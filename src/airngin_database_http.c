#include "airngin_database.h"
#include "airngin_diag.h"
#include "airngin_db_root_ca.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

/*
 * Production binding of the Cloud Database: ONE esp_http_client exchange per request on a short-lived worker task.
 *   - TLS 1.2/1.3 with the embedded ISRG Root X1 trust anchor and standard hostname verification.
 *     Nothing is weakened: no insecure flag and no skipped CN/hostname check.
 *   - Redirects are NOT followed (the request carries the device password; it must never be re-sent elsewhere).
 *   - The whole exchange is bounded by timeout_ms of wall-clock time and can be cancelled from another task.
 *   - No retries, ever.
 * Do not enable ESP_LOG debug output of the HTTP client: it prints the URL, and GET/DELETE carry the credentials in it.
 */
static const char *TAG = "airngin_db_http";

typedef struct {
    char *data;
    size_t len;
    size_t cap_limit;
    int64_t deadline_us;
    bool overflow;
    bool timed_out;
    esp_http_client_handle_t client;
} http_ctx;

static volatile esp_http_client_handle_t s_client;      /* the in-flight client, for cancel() */
static volatile bool s_cancel;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    http_ctx *ctx = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !ctx || ctx->overflow) return ESP_OK;
    if (esp_timer_get_time() > ctx->deadline_us) {
        ctx->timed_out = true;
        esp_http_client_cancel_request(ctx->client);
        return ESP_OK;
    }
    if (ctx->len + (size_t)evt->data_len > ctx->cap_limit) {
        ctx->overflow = true;
        esp_http_client_cancel_request(ctx->client);
        return ESP_OK;
    }
    char *grown = realloc(ctx->data, ctx->len + (size_t)evt->data_len + 1U);
    if (!grown) {
        ctx->overflow = true;
        return ESP_OK;
    }
    ctx->data = grown;
    memcpy(ctx->data + ctx->len, evt->data, (size_t)evt->data_len);
    ctx->len += (size_t)evt->data_len;
    ctx->data[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t http_perform(const airngin_db_http_request_t *req, uint32_t timeout_ms, size_t response_cap,
                              char **response, size_t *response_len, int *http_status)
{
    http_ctx ctx = { .cap_limit = response_cap, .deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL };
    *response = NULL;
    *response_len = 0;
    *http_status = 0;
    s_cancel = false;

    esp_http_client_config_t cfg = {
        .host = AIRNGIN_DATABASE_HOST,
        .path = req->path,
        .query = req->query,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .method = strcmp(req->method, "DELETE") == 0 ? HTTP_METHOD_DELETE : HTTP_METHOD_POST,
        .timeout_ms = (int)timeout_ms,
        .cert_pem = (const char *)airngin_db_root_ca_start,
        .event_handler = on_event,
        .user_data = &ctx,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    ctx.client = client;
    s_client = client;
    if (s_cancel) { s_client = NULL; esp_http_client_cleanup(client); return ESP_ERR_INVALID_STATE; }

    esp_err_t err = ESP_OK;
    if (req->content_type) err = esp_http_client_set_header(client, "Content-Type", req->content_type);
    if (err == ESP_OK && req->body) err = esp_http_client_set_post_field(client, req->body, (int)req->body_len);
    if (err == ESP_OK) err = esp_http_client_perform(client);
    if (err == ESP_OK) *http_status = esp_http_client_get_status_code(client);
    s_client = NULL;
    esp_http_client_cleanup(client);

    if (s_cancel) {
        err = ESP_ERR_INVALID_STATE;
    } else if (ctx.timed_out || err == ESP_ERR_HTTP_EAGAIN || err == ESP_ERR_TIMEOUT) {
        err = ESP_ERR_TIMEOUT;
    } else if (ctx.overflow) {
        err = ESP_ERR_NO_MEM;                      /* reply larger than the SDK bound */
    }
    if (err != ESP_OK) {
        free(ctx.data);
        ctx.data = NULL;
        ctx.len = 0;
        return err;
    }
    *response = ctx.data;
    *response_len = ctx.len;
    return ESP_OK;
}

static void http_cancel(void)
{
    s_cancel = true;
    esp_http_client_handle_t client = s_client;
    if (client) esp_http_client_cancel_request(client);
}

typedef struct { void (*fn)(void *); void *arg; } worker_arg;

static void worker_task(void *p)
{
    worker_arg wa = *(worker_arg *)p;
    free(p);
    wa.fn(wa.arg);
    vTaskDelete(NULL);
}

static esp_err_t http_submit(void (*fn)(void *), void *arg)
{
    worker_arg *wa = malloc(sizeof(*wa));
    if (!wa) return ESP_ERR_NO_MEM;
    wa->fn = fn;
    wa->arg = arg;
    if (xTaskCreate(worker_task, "airngin_db", CONFIG_AIRNGIN_DATABASE_TASK_STACK_SIZE, wa, 4, NULL) != pdPASS) {
        free(wa);
        ESP_LOGW(TAG, "unable to create the worker task (free heap=%u)", (unsigned)esp_get_free_heap_size());
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static const airngin_db_ops_t s_ops = { .submit = http_submit, .perform = http_perform, .cancel = http_cancel };
const airngin_db_ops_t *airngin_database_platform_ops(void) { return &s_ops; }

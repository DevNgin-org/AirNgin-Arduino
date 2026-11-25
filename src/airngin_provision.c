#include "airngin.h"
#include "airngin_runtime_ops.h"
#include "airngin_profile.h"
#include "airngin_transport.h"
#include "airngin_identity.h"
#include "airngin_clock.h"
#include "airngin_provision_query.h"
#include "airngin_session.h"
#include "airngin_prov_fsm.h"
#include "airngin_prov_mqtt_gate.h"
#include "airngin_diag.h"
#include "AirNginConfig.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/* Arduino port: the provisioning HTTP endpoint uses a small dedicated BSD-socket server.
 * This preserves long GET /set?data=... requests without depending on the prebuilt
 * Arduino core's CONFIG_HTTPD_MAX_URI_LEN (normally 512 bytes). */


#ifndef CONFIG_AIRNGIN_TASK_STACK_SIZE
#define CONFIG_AIRNGIN_TASK_STACK_SIZE 16384
#endif

#define PV_SESSION_TTL_US    120000000LL
#define PV_STOP_WAIT_MS      10000

#define PV_FLAG_PROVISIONED  BIT0
#define PV_FLAG_WIFI_STOPPED BIT1
#define PV_FLAG_STOP_REQUEST BIT2
#define PV_FLAG_OWNER_PARKED BIT3  /* owner finished touching shared state; safe to delete it */
#define PV_FLAG_START        BIT4  /* releases the gated owner task */
#define PV_FLAG_HTTP_STOPPED BIT5  /* Arduino provisioning HTTP task exited */

static const char *TAG = "airngin_provision";
static int pv_http_listen_fd = -1;
static TaskHandle_t pv_http_task_handle;
static volatile bool pv_http_stop_requested;
static esp_netif_t *pv_ap, *pv_sta;
static TaskHandle_t pv_owner;
static EventGroupHandle_t pv_flags;
static esp_event_handler_instance_t pv_we, pv_ie;
static airngin_profile pv_profile;
static bool pv_saved, pv_recovery, pv_wifi, pv_started;
/* Informational only: the default event loop is process-lifetime shared infrastructure
 * and is never deleted by AirNgin, even if AirNgin created it. */
static bool pv_loop_created;
static portMUX_TYPE pv_lock = portMUX_INITIALIZER_UNLOCKED;
static airngin_session_t pv_session;   /* guarded by pv_lock */
static bool pv_ip, pv_seen, pv_lost, pv_restored, pv_panel_notice;
static bool pv_provisioned;          /* a valid profile is stored (guarded by pv_lock) */
static bool pv_manual_request;       /* airngin_request_provisioning() pending (guarded by pv_lock) */
static bool pv_cancel_request;       /* airngin_cancel_provisioning() pending (guarded by pv_lock) */
static int pv_wifi_reason;           /* last STA disconnect reason (guarded by pv_lock) */
static bool pv_panel_open;           /* owner/start/cleanup context only */
static int64_t pv_restored_at, pv_retry;
#if AG_DIAG
#include "airngin_wifi_reason.inc"
/* Why the panel is being opened (set by the caller just before pv_open_panel; never guessed). */
static const char *pv_open_reason = "unknown";
#endif

static esp_err_t pv_http_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len) {
        const int n = send(fd, p, len, 0);
        if (n <= 0) return ESP_FAIL;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t pv_reply_fd(int fd, const char *status, const char *body)
{
    char header[256];
    const size_t body_len = strlen(body);
    const int n = snprintf(header, sizeof(header),
                           "HTTP/1.1 %s\r\nContent-Type: application/json\r\n"
                           "Content-Length: %u\r\nConnection: close\r\n\r\n",
                           status, (unsigned)body_len);
    if (n <= 0 || (size_t)n >= sizeof(header)) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = pv_http_send_all(fd, header, (size_t)n);
    if (err == ESP_OK) err = pv_http_send_all(fd, body, body_len);
    return err;
}

static esp_err_t pv_health_fd(int fd)
{
    AG_DIAG_I("AG_PROV", "HTTP GET /health");
    char body[96];
    snprintf(body, sizeof(body), "{\"status\":\"ok\",\"serial\":\"%s\"}", airngin_internal_serial());
    return pv_reply_fd(fd, "200 OK", body);
}

static esp_err_t pv_reject_fd(int fd, airngin_session_verdict_t verdict)
{
    AG_DIAG_W("AG_PROV", "409 request rejected: %s", verdict == AIRNGIN_SESSION_CLAIMED
              ? "session already claimed by a successful configuration (restart pending)"
              : "no open provisioning session (panel closed or 120 s session expired)");
    return pv_reply_fd(fd, "409 Conflict",
                       verdict == AIRNGIN_SESSION_CLAIMED
                           ? "{\"status\":\"error\",\"message\":\"session claimed\"}"
                           : "{\"status\":\"error\",\"message\":\"session unavailable\"}");
}

static esp_err_t pv_process_payload_fd(int fd, const char *body, size_t length, const char *method)
{
    portENTER_CRITICAL(&pv_lock);
    airngin_session_verdict_t verdict = airngin_session_check(&pv_session, esp_timer_get_time());
    portEXIT_CRITICAL(&pv_lock);
    if (verdict != AIRNGIN_SESSION_ADMIT) return pv_reject_fd(fd, verdict);

#if AG_DIAG
    ESP_LOGI("AG_PROV", "========== PROVISIONING RAW RX ==========");
    ESP_LOGI("AG_PROV", "Method       : %s", method);
    ESP_LOGI("AG_PROV", "URI          : /set");
    ESP_LOGI("AG_PROV", "Payload len  : %u", (unsigned)length);
    airngin_diag_dump_raw("AG_PROV", "Raw payload", body, length);
    ESP_LOGI("AG_PROV", "=========================================");
#endif

    const char *end = NULL;
    cJSON *syntax = cJSON_ParseWithLengthOpts(body, length, &end, false);
    bool good = syntax != NULL && !memchr(body, 0, length);
    if (syntax) {
        while (end < body + length && isspace((unsigned char)*end)) ++end;
        good = good && end == body + length;
        cJSON_Delete(syntax);
    }
    if (!good) {
        AG_DIAG_W("AG_PROV", "400 malformed request: invalid JSON document");
        return pv_reply_fd(fd, "400 Bad Request", "{\"status\":\"error\",\"message\":\"malformed request\"}");
    }

    airngin_profile profile = {0};
    esp_err_t err = airngin_profile_parse(body, (unsigned)length, &profile);
    if (err != ESP_OK) {
        AG_DIAG_W("AG_PROV", "422 invalid configuration: payload validation failed " AG_ERRF, AG_ERRV(err));
        return pv_reply_fd(fd, "422 Unprocessable Entity",
                           "{\"status\":\"error\",\"message\":\"invalid or unsaved configuration\"}");
    }
#if AG_DIAG
    ESP_LOGI("AG_PROV", "device serial (configured, not part of the payload) = %s", airngin_internal_serial());
    airngin_diag_log_profile("AG_PROV", "PROVISIONING PARSED", &profile);
#endif

    portENTER_CRITICAL(&pv_lock);
    verdict = airngin_session_begin_save(&pv_session, esp_timer_get_time());
    portEXIT_CRITICAL(&pv_lock);
    if (verdict != AIRNGIN_SESSION_ADMIT) {
        airngin_secure_zero(&profile, sizeof(profile));
        return pv_reject_fd(fd, verdict);
    }
#if AG_DIAG
    ESP_LOGI("AG_PROV", "no normalization happens between parse and store: profile below is written byte-for-byte");
    airngin_diag_log_profile("AG_PROV", "PROVISIONING FINAL PROFILE", &profile);
#endif
    err = airngin_profile_save(&profile);
    airngin_secure_zero(&profile, sizeof(profile));
    portENTER_CRITICAL(&pv_lock);
    airngin_session_end_save(&pv_session, err == ESP_OK);
    if (err == ESP_OK) pv_provisioned = true;
    portEXIT_CRITICAL(&pv_lock);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_PROV", "422 profile NOT saved: NVS write/readback failed " AG_ERRF, AG_ERRV(err));
        return pv_reply_fd(fd, "422 Unprocessable Entity",
                           "{\"status\":\"error\",\"message\":\"invalid or unsaved configuration\"}");
    }

    AG_DIAG_I("AG_PROV", "========== Provisioning Success ==========");
    AG_DIAG_I("AG_PROV", "Profile validated     : YES");
    AG_DIAG_I("AG_PROV", "NVS write             : PASS");
    AG_DIAG_I("AG_PROV", "Readback verification : PASS");
    AG_DIAG_I("AG_PROV", "Restart scheduled     : 3000 ms");
    AG_DIAG_I("AG_PROV", "===========================================");
    const esp_err_t sent = pv_reply_fd(fd, "200 OK", "{\"status\":\"ok\"}");
    xEventGroupSetBits(pv_flags, PV_FLAG_PROVISIONED);
    return sent;
}

static long pv_content_length(const char *headers)
{
    const char *p = headers;
    while (p && *p) {
        const char *e = strstr(p, "\r\n");
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 15U && strncasecmp(p, "Content-Length:", 15U) == 0) {
            const char *v = p + 15U;
            while (*v == ' ' || *v == '\t') ++v;
            char *end = NULL;
            const long value = strtol(v, &end, 10);
            return (end != v && value >= 0) ? value : -1;
        }
        if (!e) break;
        p = e + 2;
    }
    return -1;
}

static void pv_http_handle_client(int fd)
{
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    const size_t cap = AIRNGIN_PROVISION_QUERY_MAX + 2048U;
    char *request = malloc(cap + 1U);
    if (!request) { (void)pv_reply_fd(fd, "500 Internal Server Error", "{\"status\":\"error\"}"); return; }
    size_t used = 0;
    char *header_end = NULL;
    while (used < cap) {
        const int n = recv(fd, request + used, cap - used, 0);
        if (n <= 0) break;
        used += (size_t)n;
        request[used] = 0;
        header_end = strstr(request, "\r\n\r\n");
        if (header_end) break;
    }
    if (!header_end) {
        (void)pv_reply_fd(fd, "400 Bad Request", "{\"status\":\"error\",\"message\":\"malformed request\"}");
        free(request); return;
    }

    char *line_end = strstr(request, "\r\n");
    if (!line_end) { free(request); return; }
    *line_end = 0;
    char *method = request;
    char *sp1 = strchr(method, ' ');
    if (!sp1) { free(request); return; }
    *sp1++ = 0;
    char *target = sp1;
    char *sp2 = strchr(target, ' ');
    if (!sp2) { free(request); return; }
    *sp2 = 0;
    char *headers = line_end + 2;
    char *body_start = header_end + 4;
    const size_t body_have = used - (size_t)(body_start - request);

    if (!strcmp(method, "GET") && !strcmp(target, "/health")) {
        (void)pv_health_fd(fd);
        free(request); return;
    }

    if (!strcmp(method, "GET") && !strncmp(target, "/set?", 5U)) {
        const char *query = target + 5U;
        const size_t query_len = strlen(query);
        char *decoded = calloc(1, AIRNGIN_PROVISION_JSON_MAX + 1U);
        size_t decoded_len = 0;
        if (!decoded) { free(request); return; }
#if AG_DIAG
        ESP_LOGI("AG_PROV", "HTTP GET /set");
        airngin_diag_dump_raw("AG_PROV", "Raw encoded query", query, query_len);
#endif
        if (!airngin_provision_decode_query(query, query_len, decoded,
                                            AIRNGIN_PROVISION_JSON_MAX + 1U, &decoded_len)) {
            (void)pv_reply_fd(fd, "400 Bad Request", "{\"status\":\"error\",\"message\":\"malformed request\"}");
        } else {
            (void)pv_process_payload_fd(fd, decoded, decoded_len, "GET");
        }
        airngin_secure_zero(decoded, AIRNGIN_PROVISION_JSON_MAX + 1U);
        free(decoded); free(request); return;
    }

    if (!strcmp(method, "POST") && !strcmp(target, "/set")) {
        const long content_len = pv_content_length(headers);
        if (content_len <= 0 || content_len > (long)AIRNGIN_PROVISION_JSON_MAX) {
            (void)pv_reply_fd(fd, "400 Bad Request", "{\"status\":\"error\",\"message\":\"malformed request\"}");
            free(request); return;
        }
        char *body = calloc(1, (size_t)content_len + 1U);
        if (!body) { free(request); return; }
        size_t copied = body_have > (size_t)content_len ? (size_t)content_len : body_have;
        if (copied) memcpy(body, body_start, copied);
        while (copied < (size_t)content_len) {
            const int n = recv(fd, body + copied, (size_t)content_len - copied, 0);
            if (n <= 0) break;
            copied += (size_t)n;
        }
        if (copied != (size_t)content_len) {
            (void)pv_reply_fd(fd, "400 Bad Request", "{\"status\":\"error\",\"message\":\"malformed request\"}");
        } else {
            (void)pv_process_payload_fd(fd, body, copied, "POST");
        }
        airngin_secure_zero(body, (size_t)content_len + 1U);
        free(body); free(request); return;
    }

    (void)pv_reply_fd(fd, "404 Not Found", "{\"status\":\"error\",\"message\":\"not found\"}");
    free(request);
}

static void pv_http_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (pv_http_stop_requested) break;
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(pv_http_listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (pv_http_stop_requested) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        pv_http_handle_client(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    /* Signal completion before clearing the handle so cleanup cannot delete
     * pv_flags while this task is still about to touch the event group. */
    if (pv_flags) xEventGroupSetBits(pv_flags, PV_FLAG_HTTP_STOPPED);
    pv_http_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t pv_http_start(void)
{
    if (pv_http_task_handle || pv_http_listen_fd >= 0) return ESP_ERR_INVALID_STATE;
    pv_http_stop_requested = false;
    xEventGroupClearBits(pv_flags, PV_FLAG_HTTP_STOPPED);
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return ESP_FAIL;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = inet_addr("192.168.1.1");
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd); return ESP_FAIL;
    }
    pv_http_listen_fd = fd;
    if (xTaskCreate(pv_http_task, "airngin_http", 16384, NULL, 4, &pv_http_task_handle) != pdPASS) {
        close(fd); pv_http_listen_fd = -1; return ESP_ERR_NO_MEM;
    }
    AG_DIAG_I("AG_PROV", "Arduino provisioning HTTP server started on 192.168.1.1:80");
    return ESP_OK;
}

static esp_err_t pv_http_stop(void)
{
    if (!pv_http_task_handle && pv_http_listen_fd < 0) return ESP_OK;
    pv_http_stop_requested = true;
    const bool had_task = pv_http_task_handle != NULL;
    const int fd = pv_http_listen_fd;
    pv_http_listen_fd = -1;
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    if (had_task && pv_flags) {
        const EventBits_t bits = xEventGroupWaitBits(pv_flags, PV_FLAG_HTTP_STOPPED, pdFALSE, pdTRUE, pdMS_TO_TICKS(6000));
        if (!(bits & PV_FLAG_HTTP_STOPPED)) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void pv_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    const int64_t event_time = esp_timer_get_time();
#if AG_DIAG
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            AG_DIAG_I("AG_WIFI", "STA_START");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *c = data;
            if (c) AG_DIAG_I("AG_WIFI", "STA_CONNECTED ssid=\"%.*s\" channel=%u authmode=%d", (int)c->ssid_len,
                             (const char *)c->ssid, (unsigned)c->channel, (int)c->authmode);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *d = data;
            if (d) AG_DIAG_W("AG_WIFI", "STA_DISCONNECTED reason=%d (%s)", (int)d->reason, airngin_wifi_reason_name(d->reason));
            break;
        }
        case WIFI_EVENT_STA_STOP:
            AG_DIAG_I("AG_WIFI", "STA_STOP");
            break;
        case WIFI_EVENT_AP_START:
            AG_DIAG_I("AG_WIFI", "AP_START");
            break;
        case WIFI_EVENT_AP_STOP:
            AG_DIAG_I("AG_WIFI", "AP_STOP");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *c = data;
            if (c) AG_DIAG_I("AG_WIFI", "AP client connected mac=" MACSTR " aid=%d", MAC2STR(c->mac), (int)c->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *c = data;
            if (c) AG_DIAG_I("AG_WIFI", "AP client disconnected mac=" MACSTR " aid=%d", MAC2STR(c->mac), (int)c->aid);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP && data) {
            const ip_event_got_ip_t *g = data;
            AG_DIAG_I("AG_WIFI", "GOT_IP ip=" IPSTR " mask=" IPSTR " gw=" IPSTR, IP2STR(&g->ip_info.ip),
                      IP2STR(&g->ip_info.netmask), IP2STR(&g->ip_info.gw));
        } else if (id == IP_EVENT_STA_LOST_IP) {
            AG_DIAG_W("AG_WIFI", "LOST_IP");
        }
    }
#endif

    portENTER_CRITICAL(&pv_lock);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        pv_ip = true;
        if (pv_seen && pv_lost) {
            pv_restored = true;
            pv_restored_at = event_time;
        }
        pv_seen = true;
        pv_lost = false;
    }
    if ((base == WIFI_EVENT &&
         (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) ||
        (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
        pv_ip = false;
        if (pv_seen) pv_lost = true;
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && data) {
            pv_wifi_reason = ((const wifi_event_sta_disconnected_t *)data)->reason;
        }
    }
    portEXIT_CRITICAL(&pv_lock);

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP && pv_flags) {
        xEventGroupSetBits(pv_flags, PV_FLAG_WIFI_STOPPED);
    }
}

/* A failed stop keeps the ownership flags set so a later cleanup can retry. */
static esp_err_t pv_close_panel(void)
{
    esp_err_t first_error = ESP_OK;
    portENTER_CRITICAL(&pv_lock);
    airngin_session_close(&pv_session);
    portEXIT_CRITICAL(&pv_lock);
    if (pv_http_task_handle || pv_http_listen_fd >= 0) {
        const esp_err_t err = pv_http_stop();
        AG_DIAG_I("AG_PROV", "HTTP server stop: " AG_ERRF, AG_ERRV(err));
        if (err != ESP_OK) first_error = err;
    }
    if (!pv_http_task_handle && pv_http_listen_fd < 0) pv_panel_open = false;
    if (pv_started) {
        const esp_err_t err = esp_wifi_stop();
        AG_DIAG_I("AG_WIFI", "esp_wifi_stop: " AG_ERRF, AG_ERRV(err));
        if (err == ESP_OK) pv_started = false;
        else if (first_error == ESP_OK) first_error = err;
    }
    return first_error;
}

static esp_err_t pv_open_panel(void)
{
    esp_err_t err;
    wifi_mode_t mode;
#if AG_DIAG
    ESP_LOGI("AG_PROV", "========== Provisioning Begin ==========");
    ESP_LOGI("AG_PROV", "Reason              : %s", pv_open_reason);
    ESP_LOGI("AG_PROV", "Current state       : %s", airngin_diag_state_name(airngin_get_state()));
    ESP_LOGI("AG_PROV", "Stored profile      : %s", pv_saved ? "yes" : "no");
    ESP_LOGI("AG_PROV", "Network mode        : MANAGED_WIFI");
    ESP_LOGI("AG_PROV", "=========================================");
#endif
    if (pv_started) {
        err = esp_wifi_get_mode(&mode);
        if (err != ESP_OK) return err;
        AG_DIAG_I("AG_WIFI", "stopping the running Wi-Fi (%s) to open the provisioning AP", mode == WIFI_MODE_STA ? "STA" : "other mode");
        xEventGroupClearBits(pv_flags, PV_FLAG_WIFI_STOPPED);
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            AG_DIAG_E("AG_WIFI", "esp_wifi_stop failed " AG_ERRF, AG_ERRV(err));
            return err;
        }
        pv_started = false;
        if (mode == WIFI_MODE_STA &&
            !(xEventGroupWaitBits(pv_flags, PV_FLAG_WIFI_STOPPED, pdFALSE, pdTRUE,
                                  pdMS_TO_TICKS(5000)) & PV_FLAG_WIFI_STOPPED)) {
            AG_DIAG_E("AG_WIFI", "STA_STOP was not observed within 5000 ms");
            return ESP_ERR_TIMEOUT;
        }
    }

    AG_DIAG_I("AG_PROV", "SoftAP starting");
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_set_mode(AP) failed " AG_ERRF, AG_ERRV(err));
        return err;
    }

    wifi_config_t config = {0};
    const int ssid_chars = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid),
                                    "%s_%s", airngin_internal_serial(),
                                    airngin_internal_provisioning_label());
    if (ssid_chars < 0 || (size_t)ssid_chars >= sizeof(config.ap.ssid)) {
        return ESP_ERR_INVALID_SIZE;
    }
    config.ap.ssid_len = (uint8_t)ssid_chars;
    const char *password = airngin_internal_provisioning_password();
    memcpy(config.ap.password, password, strlen(password) + 1U);
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 4;

    err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_set_config(AP) failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_start (AP) failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    pv_started = true;
#if AG_DIAG
    {
        uint8_t channel = 0;
        wifi_second_chan_t second;
        (void)esp_wifi_get_channel(&channel, &second);
        esp_netif_ip_info_t ap_ip = {0};
        (void)esp_netif_get_ip_info(pv_ap, &ap_ip);
        ESP_LOGI("AG_PROV", "SSID=%.*s", (int)config.ap.ssid_len, (const char *)config.ap.ssid);
        ESP_LOGI("AG_PROV", "channel=%u auth=WPA2 max_connections=%u", (unsigned)channel, (unsigned)config.ap.max_connection);
        ESP_LOGI("AG_PROV", "AP password: configured, length=%u (never printed)", (unsigned)strlen(password));
        ESP_LOGI("AG_PROV", "AP IP=" IPSTR, IP2STR(&ap_ip.ip));
    }
#endif

    err = pv_http_start();
    if (err != ESP_OK) {
        AG_DIAG_E("AG_PROV", "HTTP server start failed " AG_ERRF, AG_ERRV(err));
        (void)pv_close_panel();
        return err;
    }
    AG_DIAG_I("AG_PROV", "GET /health available");
    AG_DIAG_I("AG_PROV", "GET/POST /set available");

    portENTER_CRITICAL(&pv_lock);
    airngin_session_open(&pv_session, esp_timer_get_time(), PV_SESSION_TTL_US);
    portEXIT_CRITICAL(&pv_lock);
    pv_panel_open = true;
    AG_DIAG_I("AG_PROV", "provisioning timeout=%u s", (unsigned)(PV_SESSION_TTL_US / 1000000LL));
    airngin_diag_snapshot("provisioning open", false);
    return ESP_OK;
}

static bool pv_has_resources(void)
{
    return pv_flags || pv_ap || pv_sta || pv_we || pv_ie || pv_wifi || pv_started || pv_http_task_handle || pv_http_listen_fd >= 0;
}

/*
 * Tear down everything AirNgin created, in dependency order:
 *   HTTP server -> MQTT -> SNTP -> event handlers -> Wi-Fi stop -> Wi-Fi deinit
 *   -> default netifs -> event group.
 * Every step is conditional on its own ownership handle/flag and an ownership
 * flag is cleared ONLY when the corresponding teardown succeeded. A failing step
 * stops the sequence (dependent resources must not be destroyed while a
 * prerequisite is still live) and its error is returned; the function is safe to
 * call again to resume. Must run only when the owner task is parked or absent.
 * The default event loop, NVS and esp-netif core are shared and left untouched.
 */
static esp_err_t pv_cleanup(bool delete_flags)
{
    esp_err_t err;

    if (pv_http_task_handle || pv_http_listen_fd >= 0) {
        err = pv_http_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server stop: " AG_ERRF, AG_ERRV(err));
            return err;
        }
        AG_DIAG_I("AG_PROV", "cleanup: HTTP server stopped");
    }
    portENTER_CRITICAL(&pv_lock);
    airngin_session_close(&pv_session);
    portEXIT_CRITICAL(&pv_lock);

    AG_DIAG_I("AG_MQTT", "cleanup: MQTT client teardown");
    err = airngin_transport_destroy();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT teardown: " AG_ERRF, AG_ERRV(err));
        return err;
    }

    airngin_clock_stop();   /* SNTP session owned by AirNgin; idempotent */
    AG_DIAG_I("AG_TIME", "cleanup: SNTP stopped");

    esp_err_t handler_error = ESP_OK;
    if (pv_we) {
        err = esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, pv_we);
        if (err == ESP_OK) pv_we = NULL;
        else handler_error = err;
    }
    if (pv_ie) {
        err = esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, pv_ie);
        if (err == ESP_OK) pv_ie = NULL;
        else if (handler_error == ESP_OK) handler_error = err;
    }
    if (handler_error != ESP_OK) {
        /* pv_event still references pv_flags: keep everything alive and retry later. */
        ESP_LOGW(TAG, "event handler unregister: " AG_ERRF, AG_ERRV(handler_error));
        return handler_error;
    }
    AG_DIAG_I("AG_WIFI", "cleanup: Wi-Fi/IP event handlers unregistered");

    if (pv_started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_stop: " AG_ERRF, AG_ERRV(err));
            return err;
        }
        pv_started = false;
        AG_DIAG_I("AG_WIFI", "cleanup: Wi-Fi stopped");
    }
    if (pv_wifi) {
        err = esp_wifi_deinit();
        if (err != ESP_OK) {
            /* The driver may still use the netifs: do NOT destroy them. */
            ESP_LOGW(TAG, "esp_wifi_deinit: " AG_ERRF, AG_ERRV(err));
            return err;
        }
        pv_wifi = false;
        AG_DIAG_I("AG_WIFI", "cleanup: Wi-Fi driver deinitialized");
    }

    if (pv_ap) {
        esp_netif_destroy_default_wifi(pv_ap);
        pv_ap = NULL;
    }
    if (pv_sta) {
        esp_netif_destroy_default_wifi(pv_sta);
        pv_sta = NULL;
        AG_DIAG_I("AG_WIFI", "cleanup: default STA/AP netifs destroyed");
    }

    portENTER_CRITICAL(&pv_lock);
    pv_ip = false;
    portEXIT_CRITICAL(&pv_lock);

    if (delete_flags && pv_flags) {
        vEventGroupDelete(pv_flags);
        pv_flags = NULL;
    }
    return ESP_OK;
}

/*
 * Safe point for task termination. After the PARKED bit is set the owner never
 * touches pv_flags or any other shared state again; it blocks until the stopping
 * task deletes it (airngin_managed_stop). This is the join: shared primitives are
 * deleted only after the owner has provably finished using them.
 */
static void pv_park(void)
{
    xEventGroupSetBits(pv_flags, PV_FLAG_OWNER_PARKED);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

/* Resume station mode with the stored profile (after a manual panel session). Idempotent. */
static esp_err_t pv_resume_sta(int64_t now)
{
    AG_DIAG_I("AG_WIFI", "restoring previous STA profile ssid=\"%s\"", pv_profile.modemSsid);
    esp_err_t err = pv_close_panel();
    if (err != ESP_OK) {
        AG_DIAG_W("AG_WIFI", "STA restore postponed: panel close " AG_ERRF, AG_ERRV(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_set_mode(STA) failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, pv_profile.modemSsid, strlen(pv_profile.modemSsid));
    memcpy(config.sta.password, pv_profile.modemPass, strlen(pv_profile.modemPass));
    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_start (STA) failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    pv_started = true;
    pv_retry = now;   /* connect immediately */
    AG_DIAG_I("AG_WIFI", "STA restored; reconnecting");
    return ESP_OK;
}

static void pv_emit(airngin_event_t type, esp_err_t error, int reason)
{
    airngin_event_data_t event = { .type = type, .error = error };
    if (type == AIRNGIN_EVENT_NETWORK_CONNECTED || type == AIRNGIN_EVENT_NETWORK_DISCONNECTED) {
        event.data.network.source = AIRNGIN_NETWORK_SOURCE_MANAGED_WIFI;
        event.data.network.reason = reason;
    }
    airngin_internal_emit(&event);
}

/* ops adapters for airngin_prov_step() */
static esp_err_t pvo_stop_mqtt(void *ctx) { (void)ctx; return airngin_transport_stop(); }
static esp_err_t pvo_open(void *ctx)
{
    (void)ctx;
    AG_DIAG_DO(pv_open_reason = "application request (airngin_request_provisioning(); the SDK does not know which input triggered it)");
    return pv_open_panel();
}
static esp_err_t pvo_close(void *ctx) { (void)ctx; return pv_close_panel(); }
static esp_err_t pvo_resume(void *ctx, int64_t now) { (void)ctx; return pv_resume_sta(now); }
static void pvo_emit(void *ctx, airngin_event_t e, esp_err_t err)
{
    (void)ctx;
#if AG_DIAG
    if (e == AIRNGIN_EVENT_PROVISIONING_TIMEOUT) {
        const bool resumes = pv_saved && !pv_recovery;
        ESP_LOGI("AG_PROV", "========== Provisioning Timeout ==========");
        ESP_LOGI("AG_PROV", "New profile saved   : NO");
        ESP_LOGI("AG_PROV", "Previous profile    : %s", pv_saved ? "retained" : "none (device stays idle until a request or reboot)");
        ESP_LOGI("AG_PROV", "Returning to STA    : %s", resumes ? "YES" : "NO");
        ESP_LOGI("AG_PROV", "===========================================");
    } else if (e == AIRNGIN_EVENT_PROVISIONING_CANCELLED) {
        const bool resumes = pv_saved && !pv_recovery;
        ESP_LOGI("AG_PROV", "========== Provisioning Cancelled ==========");
        ESP_LOGI("AG_PROV", "Closed by            : airngin_cancel_provisioning()");
        ESP_LOGI("AG_PROV", "New profile saved    : NO");
        ESP_LOGI("AG_PROV", "Previous profile     : %s", pv_saved ? "retained" : "none (device stays RUNNING and idle; reopen with a new request or restart)");
        ESP_LOGI("AG_PROV", "Returning to STA     : %s", resumes ? "YES" : "NO");
        ESP_LOGI("AG_PROV", "=============================================");
    } else if (e == AIRNGIN_EVENT_PROVISIONING_FAILED) {
        ESP_LOGE("AG_PROV", "provisioning panel could not be opened " AG_ERRF, AG_ERRV(err));
    }
#endif
    pv_emit(e, err, 0);
}
static const airngin_prov_ops_t pv_ops = {
    .stop_mqtt = pvo_stop_mqtt, .open_panel = pvo_open, .close_panel = pvo_close,
    .resume_sta = pvo_resume, .emit = pvo_emit,
};

static void pv_task(void *arg)
{
    (void)arg;
    int64_t restart = 0;
    bool marking = false;
    bool ip_report_initialized = false;
    bool ip_reported = false;
    airngin_prov_fsm_t fsm = {0};

    /* Gated start: airngin_start() delivers STARTED before releasing us. */
    (void)xEventGroupWaitBits(pv_flags, PV_FLAG_START | PV_FLAG_STOP_REQUEST,
                              pdFALSE, pdFALSE, portMAX_DELAY);

    for (;;) {
        const int64_t now = esp_timer_get_time();
        const EventBits_t bits = xEventGroupGetBits(pv_flags);

        if (bits & PV_FLAG_STOP_REQUEST) {
            airngin_clock_stop();
            (void)airngin_transport_stop();
            (void)pv_close_panel();   /* residual failures are retried by pv_cleanup */
            pv_park();
        }

        portENTER_CRITICAL(&pv_lock);
        const bool ip = pv_ip;
        const bool restored = pv_restored;
        const int64_t restored_at = pv_restored_at;
        const bool panel_notice = pv_panel_notice;
        const bool session_expired = airngin_session_expired(&pv_session, now);
        const bool manual_request = pv_manual_request;
        const bool cancel_request = pv_cancel_request;
        const bool session_claimed = pv_session.claimed;
        const bool session_saving = pv_session.saving;
        const int wifi_reason = pv_wifi_reason;
        const bool has_profile = pv_saved;
        pv_restored = false;
        pv_panel_notice = false;
        portEXIT_CRITICAL(&pv_lock);

        if (panel_notice) pv_emit(AIRNGIN_EVENT_PROVISIONING_STARTED, ESP_OK, 0);

        if (!ip_report_initialized) {
            ip_report_initialized = true;
            ip_reported = ip;
            if (ip) pv_emit(AIRNGIN_EVENT_NETWORK_CONNECTED, ESP_OK, 0);
        } else if (ip != ip_reported) {
            ip_reported = ip;
            if (ip) pv_emit(AIRNGIN_EVENT_NETWORK_CONNECTED, ESP_OK, 0);
            else pv_emit(AIRNGIN_EVENT_NETWORK_DISCONNECTED, ESP_OK, wifi_reason);
        }

        airngin_clock_tick(ip);
        if (restored) airngin_transport_wifi_recovered(restored_at);

        if (bits & PV_FLAG_PROVISIONED) {
            if (!restart) {
                restart = now + 3000000LL;
                AG_DIAG_I("AG_PROV", "new profile saved: device restarts in 3000 ms");
            }
            if (now >= restart) {
                airngin_clock_stop();
                if (pv_close_panel() == ESP_OK) {
                    /* Original successful-provisioning semantics: tidy up, then reboot. */
                    (void)pv_cleanup(false);
                    AG_DIAG_I("AG_PROV", "restarting device after successful provisioning");
                    esp_restart();
                }
            }
        }

        const airngin_prov_in_t in = {
            .now_us = now, .panel_open = pv_panel_open, .session_expired = session_expired,
            .provisioned_bit = (bits & PV_FLAG_PROVISIONED) != 0, .restart_pending = restart != 0,
            .marking = marking, .recovery = pv_recovery, .has_profile = has_profile,
            .manual_request = manual_request, .cancel_request = cancel_request,
            .session_claimed = session_claimed, .session_saving = session_saving,
        };
        airngin_prov_out_t out;
        airngin_prov_step(&fsm, &in, &out, &pv_ops, NULL);
        if (out.clear_manual_request || out.clear_cancel_request) {
            portENTER_CRITICAL(&pv_lock);
            if (out.clear_manual_request) pv_manual_request = false;
            if (out.clear_cancel_request) pv_cancel_request = false;
            portEXIT_CRITICAL(&pv_lock);
        }

        if (pv_saved && !pv_recovery && !pv_panel_open && !fsm.resume_pending && !ip &&
            now >= pv_retry) {
            const esp_err_t connect_err = esp_wifi_connect();
            AG_DIAG_I("AG_WIFI", "connecting ssid=\"%s\": esp_wifi_connect " AG_ERRF " (next attempt in 60 s if still offline)",
                      pv_profile.modemSsid, AG_ERRV(connect_err));
            (void)connect_err;
            pv_retry = now + 60000000LL;
        }

        portENTER_CRITICAL(&pv_lock);
        const bool request_now = pv_manual_request;   /* live value: includes a request recorded during this iteration */
        portEXIT_CRITICAL(&pv_lock);
        if (!pv_recovery && airngin_prov_transport_tick(&fsm, &in, pv_panel_open, request_now, ip, now) == 2) {
            pv_recovery = true;
            marking = true;
            AG_DIAG_W("AG_PROV", "broker rejected the stored credentials: flagging the profile for recovery, then opening provisioning");
            (void)airngin_transport_stop();
            pv_profile.recovery = true;
            pv_retry = now;
        }

        if (marking && now >= pv_retry) {
            if (airngin_profile_save(&pv_profile) == ESP_OK) {
                AG_DIAG_DO(pv_open_reason = "credential recovery: the broker rejected the stored credentials");
                const esp_err_t opened = pv_open_panel();
                if (opened == ESP_OK) {
                    marking = false;
                    fsm.expiry_pending = false;
                    pv_emit(AIRNGIN_EVENT_PROVISIONING_STARTED, ESP_OK, 0);
                } else {
                    pv_emit(AIRNGIN_EVENT_PROVISIONING_FAILED, opened, 0);
                    pv_retry = now + 60000000LL;
                }
            } else {
                pv_retry = now + 60000000LL;
            }
        }

        airngin_diag_status_watch();   /* logs network / clock / MQTT / ready CHANGES only */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool airngin_managed_in_owner_task(void)
{
    return pv_owner != NULL && xTaskGetCurrentTaskHandle() == pv_owner;
}

void airngin_managed_get_status(airngin_runtime_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&pv_lock);
    out->network_connected = pv_ip;
    out->credentials_available = pv_provisioned;
    out->provisioning_active = pv_session.open;
    portEXIT_CRITICAL(&pv_lock);
    out->clock_trusted = airngin_clock_trusted();
    out->mqtt_connected = airngin_transport_connected();
    out->mqtt_ready = airngin_transport_ready();
    out->credentials_rejected = pv_recovery;   /* informational: credential-recovery mode (unchanged behaviour) */
    airngin_transport_counters(&out->rx_dropped, &out->tx_rejected, &out->tx_failed);
}

esp_err_t airngin_managed_publish(const char *key, size_t key_len, const char *value, size_t value_len)
{
    return airngin_transport_enqueue_publish(key, key_len, value, value_len);
}

esp_err_t airngin_managed_notify(bool to_user, int channel, const char *code, size_t code_len)
{
    return airngin_transport_enqueue_notification(to_user, channel, code, code_len);
}

esp_err_t airngin_managed_cancel_provisioning(void)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&pv_lock);
    /* Active = the panel is open and no configuration has been committed yet, or a request is still waiting to open it.
     * A committed profile (claimed) means provisioning SUCCEEDED: nothing to cancel, success wins. */
    const bool active = (pv_session.open && !pv_session.claimed) || pv_manual_request;
    if (active) pv_cancel_request = true;      /* coalesced; the owner task performs the transition */
    else err = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&pv_lock);
    return err;
}

esp_err_t airngin_managed_request_provisioning(void)
{
    portENTER_CRITICAL(&pv_lock);
    pv_manual_request = true;
    portEXIT_CRITICAL(&pv_lock);
    return ESP_OK;
}

void airngin_managed_release_owner(void)
{
    if (pv_owner && pv_flags) xEventGroupSetBits(pv_flags, PV_FLAG_START);
}

esp_err_t airngin_managed_start(void)
{
    if (!airngin_identity_valid(airngin_internal_serial(),
                                airngin_internal_producer_code())) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pv_owner) return ESP_ERR_INVALID_STATE;

    /* A previous start/stop left resources behind (cleanup failed): finish that first. */
    airngin_clock_set_external(false);   /* managed mode acquires time itself (SNTP) */
    if (pv_has_resources()) {
        const esp_err_t cleanup_err = pv_cleanup(true);
        if (cleanup_err != ESP_OK) return cleanup_err;
    }

    pv_saved = false;
    pv_recovery = false;
    memset(&pv_session, 0, sizeof(pv_session));
    pv_ip = false;
    pv_seen = false;
    pv_lost = false;
    pv_restored = false;
    pv_panel_notice = false;
    pv_manual_request = false;
    pv_cancel_request = false;
    pv_provisioned = false;
    pv_wifi_reason = 0;
    pv_panel_open = false;
    pv_retry = 0;
    pv_restored_at = 0;
    memset(&pv_profile, 0, sizeof(pv_profile));

    /*
     * NVS, esp-netif and the default event loop are shared process-wide services.
     * AirNgin may initialize them but never deinitializes, erases or deletes them.
     * In particular nvs_flash_init() errors (no free pages / new version) are
     * returned to the host: erasing the default NVS partition would destroy
     * unrelated application data.
     */
    AG_DIAG_I("AG_NET", "managed Wi-Fi runtime starting");
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        AG_DIAG_E("AG_NVS", "nvs_flash_init failed " AG_ERRF " (AirNgin never erases the host's NVS)", AG_ERRV(err));
        return err;
    }
    AG_DIAG_I("AG_NVS", "nvs_flash_init OK");
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        AG_DIAG_E("AG_NET", "esp_netif_init failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) AG_DIAG_I("AG_NET", "esp-netif already initialized by Arduino core/application");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        AG_DIAG_E("AG_NET", "default event loop creation failed " AG_ERRF, AG_ERRV(err));
        return err;
    }
    pv_loop_created = err == ESP_OK;
    if (pv_loop_created) {
        ESP_LOGI(TAG, "Created the default event loop (kept for the process lifetime)");
    }

    /*
     * Managed mode needs exclusive ownership of the Wi-Fi driver and of the default
     * STA/AP netifs. The esp_netif_create_default_wifi_*() helpers abort() on a
     * duplicate key, and a second esp_wifi_init() would corrupt the host's driver,
     * so refuse cleanly instead of adopting something we do not own.
     */
    wifi_mode_t existing_mode;
    if (esp_wifi_get_mode(&existing_mode) != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "Wi-Fi driver already initialized by the application; "
                      "AIRNGIN_NETWORK_MANAGED_WIFI requires exclusive Wi-Fi ownership");
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") ||
        esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        ESP_LOGE(TAG, "Default Wi-Fi STA/AP netif already exists and is not owned by AirNgin; "
                      "AIRNGIN_NETWORK_MANAGED_WIFI requires exclusive netif ownership");
        return ESP_ERR_INVALID_STATE;
    }

    pv_flags = xEventGroupCreate();
    if (!pv_flags) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    pv_saved = airngin_profile_load(&pv_profile) == ESP_OK;
    pv_recovery = pv_saved && pv_profile.recovery;
    AG_DIAG_I("AG_NVS", "profile loaded: stored=%s recovery_flag=%s", pv_saved ? "YES" : "NO", pv_recovery ? "YES" : "NO");
    portENTER_CRITICAL(&pv_lock);
    pv_provisioned = pv_saved;
    portEXIT_CRITICAL(&pv_lock);

    pv_ap = esp_netif_create_default_wifi_ap();
    pv_sta = esp_netif_create_default_wifi_sta();
    if (!pv_ap || !pv_sta) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    AG_DIAG_I("AG_WIFI", "STA netif created");
    AG_DIAG_I("AG_WIFI", "AP netif created");
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_WIFI", "esp_wifi_init failed " AG_ERRF, AG_ERRV(err));
        goto fail;
    }
    pv_wifi = true;
    AG_DIAG_I("AG_WIFI", "Wi-Fi driver init");

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              pv_event, NULL, &pv_we);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                              pv_event, NULL, &pv_ie);
    if (err != ESP_OK) goto fail;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) goto fail;

    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 1, 1);
    IP4_ADDR(&ip.gw, 192, 168, 1, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    err = esp_netif_dhcps_stop(pv_ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) goto fail;
    err = esp_netif_set_ip_info(pv_ap, &ip);
    if (err != ESP_OK) goto fail;
    err = esp_netif_dhcps_start(pv_ap);
    if (err != ESP_OK) goto fail;

    if (pv_saved && !pv_recovery) {
        AG_DIAG_I("AG_WIFI", "Wi-Fi mode=STA; stored profile will be used (ssid=\"%s\")", pv_profile.modemSsid);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) goto fail;
        wifi_config_t config = {0};
        memcpy(config.sta.ssid, pv_profile.modemSsid, strlen(pv_profile.modemSsid));
        memcpy(config.sta.password, pv_profile.modemPass, strlen(pv_profile.modemPass));
        err = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (err != ESP_OK) goto fail;
        err = airngin_transport_init(&pv_profile);
        if (err != ESP_OK) goto fail;
        err = esp_wifi_start();
        if (err != ESP_OK) {
            AG_DIAG_E("AG_WIFI", "esp_wifi_start (STA) failed " AG_ERRF, AG_ERRV(err));
            goto fail;
        }
        pv_started = true;
    } else {
        AG_DIAG_DO(pv_open_reason = pv_saved ? "stored profile is flagged for credential recovery (broker rejected it earlier)"
                                             : "no valid stored profile: automatic provisioning");
        AG_DIAG_I("AG_PROV", "%s", pv_saved ? "stored profile flagged for recovery: opening provisioning"
                                            : "no valid AirNgin profile: automatic provisioning required");
        err = pv_open_panel();
        if (err != ESP_OK) goto fail;
        portENTER_CRITICAL(&pv_lock);
        pv_panel_notice = true;   /* PROVISIONING_STARTED, delivered by the owner after STARTED */
        portEXIT_CRITICAL(&pv_lock);
    }

    if (xTaskCreate(pv_task, "airngin_owner", CONFIG_AIRNGIN_TASK_STACK_SIZE, NULL, 5,
                    &pv_owner) != pdPASS) {
        pv_owner = NULL;
        err = ESP_ERR_NO_MEM;
        AG_DIAG_E("AG_LIFE", "owner task creation failed (out of memory)");
        goto fail;
    }
    AG_DIAG_I("AG_LIFE", "owner task created (stack %u bytes), gated until STARTED is delivered", (unsigned)CONFIG_AIRNGIN_TASK_STACK_SIZE);
    return ESP_OK;

fail:
    {
        const esp_err_t cleanup_err = pv_cleanup(true);
        if (cleanup_err != ESP_OK) {
            ESP_LOGE(TAG, "start failed (%s) and cleanup is incomplete (%s); "
                          "the next airngin_start() will finish it",
                     esp_err_to_name(err), esp_err_to_name(cleanup_err));
        }
    }
    return err;
}

esp_err_t airngin_managed_stop(void)
{
    if (airngin_managed_in_owner_task()) return ESP_ERR_INVALID_STATE;

    if (pv_owner) {
        if (!pv_flags) return ESP_ERR_INVALID_STATE;
        xEventGroupSetBits(pv_flags, PV_FLAG_STOP_REQUEST);
        const EventBits_t bits = xEventGroupWaitBits(pv_flags, PV_FLAG_OWNER_PARKED,
                                                      pdFALSE, pdTRUE,
                                                      pdMS_TO_TICKS(PV_STOP_WAIT_MS));
        if (!(bits & PV_FLAG_OWNER_PARKED)) {
            /* Owner still busy (e.g. a long command callback). Nothing was touched:
             * the stop request stays latched and a later call resumes the wait. */
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelete(pv_owner);   /* parked: holds no lock and never touches shared state */
        pv_owner = NULL;
    }
    return pv_cleanup(true);
}

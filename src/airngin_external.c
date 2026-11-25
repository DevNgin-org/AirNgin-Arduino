/*
 * AIRNGIN_NETWORK_EXTERNAL runtime.
 *
 * The host owns the network and the time source. This file therefore contains NO Wi-Fi, netif, SNTP,
 * HTTP-server or NVS calls - it only owns the AirNgin owner task and the MQTT transport:
 *
 *   host --airngin_external_network_set_connected()--> flag + edge counter   (non-blocking, any task)
 *   owner task (every 100 ms): read flag -> NETWORK_* events -> clock trust detection -> transport tick
 */
#include "airngin_runtime_ops.h"
#include "airngin_transport.h"
#include "airngin_clock.h"
#include "airngin_diag.h"
#include "AirNginConfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

#ifndef CONFIG_AIRNGIN_TASK_STACK_SIZE
#define CONFIG_AIRNGIN_TASK_STACK_SIZE 16384
#endif

#define EX_STOP_WAIT_MS 10000
#define EX_FLAG_STOP   BIT0
#define EX_FLAG_PARKED BIT1
#define EX_FLAG_START  BIT2

static const char *EX_TAG = "airngin_external";
static TaskHandle_t ex_owner;
static EventGroupHandle_t ex_flags;

/* Credential generation of the credentials the transport currently holds. */
static volatile uint32_t ex_applied_generation;

/* Owner-task-only view of the host network and of the credentials. */
typedef struct {
    bool net_up;                  /* NETWORK_CONNECTED was announced and not yet withdrawn */
    uint32_t seen_generation;     /* host network edge counter */
    int64_t apply_retry_at;       /* credential replacement failed: try again at this time */
} ex_state_t;

static void ex_emit_network(airngin_event_t type)
{
    airngin_event_data_t event = { .type = type, .error = ESP_OK };
    event.data.network.source = AIRNGIN_NETWORK_SOURCE_EXTERNAL;
    airngin_internal_emit(&event);
}

/* Install credentials that were replaced since the transport was built. */
static void ex_apply_credentials(ex_state_t *s, int64_t now, int64_t delay_us)
{
    airngin_profile fresh;
    uint32_t generation = 0;
    if (!airngin_internal_cloud_profile(&fresh, &generation)) return;
    AG_DIAG_I("AG_EXT", "applying cloud credential generation %u: ending the old MQTT session, rebuilding client and topics", (unsigned)generation);
    const esp_err_t err = airngin_transport_replace_credentials(&fresh, now + delay_us);
    airngin_secure_zero(&fresh, sizeof(fresh));
    if (err == ESP_OK) {
        ex_applied_generation = generation;    /* from now on only this generation may become READY */
        AG_DIAG_I("AG_EXT", "generation %u active; MQTT may start %lld ms from now if the network is up and the clock trusted",
                  (unsigned)generation, (long long)(delay_us / 1000));
        airngin_diag_snapshot("after credential rotation", true);
    } else {
        s->apply_retry_at = now + 1000000LL;   /* e.g. old session could not be stopped yet */
        ESP_LOGW(EX_TAG, "credential replacement not applied yet: " AG_ERRF, AG_ERRV(err));
    }
}

/*
 * One owner-task iteration. Returns the transport result (2 = credentials rejected).
 *
 * Network edges: a changed edge counter while the network was up means the old IP session is over -
 * even if the host is "up" again by now (quick down/up) - so the MQTT session is dropped (queued TX is
 * discarded) and, on "up", nothing starts before now + the EXTERNAL reconnect delay. That single hold time
 * replaces any older one, so a reconnect scheduled for an earlier "up" can never start a stale session.
 * Host network loss is NOT broker failure: a broker failure while the network stays up keeps the normal
 * 60 s MQTT back-off inside airngin_transport_tick().
 *
 * Credentials: a changed generation replaces the transport's client/topics before anything else can connect.
 */
static int ex_step(ex_state_t *s, int64_t now)
{
    bool connected = false;
    uint32_t generation = 0;
    airngin_internal_external_network(&connected, &generation);
    const int64_t delay_us = (int64_t)airngin_internal_external_reconnect_delay_ms() * 1000LL;

    if (generation != s->seen_generation) {
        s->seen_generation = generation;
        if (s->net_up) {
            AG_DIAG_W("AG_EXT", "host network went DOWN: dropping the MQTT session (queued publishes discarded)");
            (void)airngin_transport_drop_session();
            s->net_up = false;
            ex_emit_network(AIRNGIN_EVENT_NETWORK_DISCONNECTED);
        }
    }
    if (connected && !s->net_up) {
        s->net_up = true;
        ex_emit_network(AIRNGIN_EVENT_NETWORK_CONNECTED);
        airngin_transport_hold_until(now + delay_us);
        AG_DIAG_I("AG_EXT", "host network is UP: MQTT/TLS may start in %lld ms (and only with a trusted clock)", (long long)(delay_us / 1000));
    }

    if (airngin_internal_cloud_generation() != ex_applied_generation && now >= s->apply_retry_at) {
        ex_apply_credentials(s, now, delay_us);
    }

    airngin_clock_tick(s->net_up);   /* external mode: trust DETECTION only, no SNTP */
    const int result = airngin_transport_tick(s->net_up, now);
    if (result == 2) {
#if AG_DIAG
        static uint32_t logged_generation;
        if (logged_generation != ex_applied_generation) {
            logged_generation = ex_applied_generation;
            AG_DIAG_W("AG_EXT", "credential generation %u rejected by the broker: suspended, no retries; "
                      "install new credentials with airngin_external_set_cloud_credentials()", (unsigned)ex_applied_generation);
        }
#endif
        airngin_internal_credentials_rejected(ex_applied_generation);   /* suspended until new credentials */
    } else if (airngin_transport_ready()) {
        airngin_internal_credentials_accepted(ex_applied_generation);
    }
    airngin_diag_status_watch();   /* logs network / clock / MQTT / ready CHANGES only */
    return result;
}

static void ex_park(void)
{
    xEventGroupSetBits(ex_flags, EX_FLAG_PARKED);
    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void ex_task(void *arg)
{
    (void)arg;
    ex_state_t state = {0};
    (void)xEventGroupWaitBits(ex_flags, EX_FLAG_START | EX_FLAG_STOP, pdFALSE, pdFALSE, portMAX_DELAY);
    for (;;) {
        if (xEventGroupGetBits(ex_flags) & EX_FLAG_STOP) {
            airngin_clock_stop();
            (void)airngin_transport_stop();
            ex_park();
        }
        /* A rejection leaves the transport suspended (CREDENTIALS_REJECTED was emitted); with
         * config-supplied credentials the host must correct them and re-init. */
        (void)ex_step(&state, esp_timer_get_time());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool ex_has_resources(void)
{
    return ex_flags != NULL;
}

/* Only AirNgin-owned things: MQTT transport and the synchronization object. */
static esp_err_t ex_cleanup(void)
{
    esp_err_t err = airngin_transport_destroy();
    if (err != ESP_OK) {
        ESP_LOGW(EX_TAG, "MQTT teardown: %s", esp_err_to_name(err));
        return err;
    }
    airngin_clock_stop();
    if (ex_flags) {
        vEventGroupDelete(ex_flags);
        ex_flags = NULL;
    }
    return ESP_OK;
}

esp_err_t airngin_external_start(void)
{
    if (ex_owner) return ESP_ERR_INVALID_STATE;
    if (ex_has_resources()) {
        const esp_err_t cleanup_err = ex_cleanup();
        if (cleanup_err != ESP_OK) return cleanup_err;
    }

    airngin_profile cloud;
    uint32_t generation = 0;
    if (!airngin_internal_cloud_profile(&cloud, &generation)) return ESP_ERR_INVALID_STATE;
    AG_DIAG_I("AG_EXT", "external-network runtime starting: the host owns Wi-Fi/Ethernet/PPP, netifs and time; AirNgin owns MQTT/TLS only");
    airngin_clock_set_external(true);
    esp_err_t err = airngin_transport_init(&cloud);
    airngin_secure_zero(&cloud, sizeof(cloud));   /* the transport keeps its own copies */
    if (err != ESP_OK) return err;
    ex_applied_generation = generation;           /* a replacement that races this start is applied by the owner */
    AG_DIAG_I("AG_EXT", "credential generation %u installed in the transport", (unsigned)generation);

    ex_flags = xEventGroupCreate();
    if (!ex_flags) {
        err = ESP_ERR_NO_MEM;
    } else if (xTaskCreate(ex_task, "airngin_owner", CONFIG_AIRNGIN_TASK_STACK_SIZE, NULL, 5,
                           &ex_owner) != pdPASS) {
        ex_owner = NULL;
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) (void)ex_cleanup();
    return err;
}

void airngin_external_release_owner(void)
{
    if (ex_owner && ex_flags) xEventGroupSetBits(ex_flags, EX_FLAG_START);
}

bool airngin_external_in_owner_task(void)
{
    return ex_owner != NULL && xTaskGetCurrentTaskHandle() == ex_owner;
}

esp_err_t airngin_external_stop(void)
{
    if (airngin_external_in_owner_task()) return ESP_ERR_INVALID_STATE;
    if (ex_owner) {
        if (!ex_flags) return ESP_ERR_INVALID_STATE;
        xEventGroupSetBits(ex_flags, EX_FLAG_STOP);
        const EventBits_t bits = xEventGroupWaitBits(ex_flags, EX_FLAG_PARKED, pdFALSE, pdTRUE,
                                                      pdMS_TO_TICKS(EX_STOP_WAIT_MS));
        if (!(bits & EX_FLAG_PARKED)) return ESP_ERR_TIMEOUT;   /* retryable, nothing touched */
        vTaskDelete(ex_owner);
        ex_owner = NULL;
    }
    return ex_cleanup();   /* the host network is left exactly as it was */
}

void airngin_external_get_status(airngin_runtime_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    bool connected = false;
    airngin_internal_external_network(&connected, NULL);
    out->network_connected = connected;          /* what the host reported */
    out->credentials_available = true;           /* validated at airngin_init() / replacement */
    out->provisioning_active = false;
    out->clock_trusted = airngin_clock_trusted();
    /* A replacement that the owner has not applied yet makes any session stale: never report it as up. */
    const bool pending = airngin_internal_cloud_generation() != ex_applied_generation;
    out->mqtt_connected = !pending && airngin_transport_connected();
    out->mqtt_ready = !pending && airngin_transport_ready();
    out->credentials_rejected = airngin_internal_credentials_rejected_flag();
    airngin_transport_counters(&out->rx_dropped, &out->tx_rejected, &out->tx_failed);
}

esp_err_t airngin_external_notify(bool to_user, int channel, const char *code, size_t code_len)
{
    bool connected = false;
    airngin_internal_external_network(&connected, NULL);
    if (!connected) return ESP_ERR_INVALID_STATE;
    if (airngin_internal_cloud_generation() != ex_applied_generation) return ESP_ERR_INVALID_STATE;
    return airngin_transport_enqueue_notification(to_user, channel, code, code_len);
}

esp_err_t airngin_external_publish(const char *key, size_t key_len, const char *value, size_t value_len)
{
    bool connected = false;
    airngin_internal_external_network(&connected, NULL);
    if (!connected) return ESP_ERR_INVALID_STATE;   /* react to the host's word immediately */
    if (airngin_internal_cloud_generation() != ex_applied_generation) return ESP_ERR_INVALID_STATE;   /* replacement pending */
    return airngin_transport_enqueue_publish(key, key_len, value, value_len);
}

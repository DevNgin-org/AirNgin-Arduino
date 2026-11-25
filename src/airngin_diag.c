#include "airngin_diag.h"

#if CONFIG_AIRNGIN_DIAGNOSTIC_LOGS

#include "airngin_diag_text.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG __attribute__((unused)) = "AG_DIAG";   /* only used by the secret warning / snapshots */

const char *airngin_diag_state_name(airngin_state_t state)
{
    switch (state) {
    case AIRNGIN_STATE_UNINITIALIZED: return "UNINITIALIZED";
    case AIRNGIN_STATE_INITIALIZED:   return "INITIALIZED";
    case AIRNGIN_STATE_STARTING:      return "STARTING";
    case AIRNGIN_STATE_RUNNING:       return "RUNNING";
    case AIRNGIN_STATE_STOPPING:      return "STOPPING";
    default:                          return "?";
    }
}

const char *airngin_diag_event_name(airngin_event_t event)
{
    switch (event) {
    case AIRNGIN_EVENT_STARTED:               return "STARTED";
    case AIRNGIN_EVENT_STOPPED:               return "STOPPED";
    case AIRNGIN_EVENT_PROVISIONING_STARTED:  return "PROVISIONING_STARTED";
    case AIRNGIN_EVENT_PROVISIONING_STOPPED:  return "PROVISIONING_STOPPED";
    case AIRNGIN_EVENT_PROVISIONING_SUCCESS:  return "PROVISIONING_SUCCESS";
    case AIRNGIN_EVENT_PROVISIONING_TIMEOUT:  return "PROVISIONING_TIMEOUT";
    case AIRNGIN_EVENT_PROVISIONING_FAILED:   return "PROVISIONING_FAILED";
    case AIRNGIN_EVENT_NETWORK_CONNECTED:     return "NETWORK_CONNECTED";
    case AIRNGIN_EVENT_NETWORK_DISCONNECTED:  return "NETWORK_DISCONNECTED";
    case AIRNGIN_EVENT_MQTT_READY:            return "MQTT_READY";
    case AIRNGIN_EVENT_MQTT_DISCONNECTED:     return "MQTT_DISCONNECTED";
    case AIRNGIN_EVENT_CREDENTIALS_REJECTED:  return "CREDENTIALS_REJECTED";
    case AIRNGIN_EVENT_PROVISIONING_CANCELLED:return "PROVISIONING_CANCELLED";
    default:                                  return "?";
    }
}

const char *airngin_diag_mode_name(airngin_network_mode_t mode)
{
    return mode == AIRNGIN_NETWORK_EXTERNAL ? "EXTERNAL" : "MANAGED_WIFI";
}

void airngin_diag_fingerprint(const char *s, size_t n, char out[9])
{
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char *)s, n, digest, 0) != 0) {
        strcpy(out, "????????");
        return;
    }
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; ++i) {
        out[2 * i] = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 15U];
    }
    out[8] = '\0';
}

void airngin_diag_field(const char *tag, const char *name, const char *value, size_t len, bool is_secret)
{
    char escaped[400];
    if (is_secret && !AG_DIAG_SECRETS) {
        char fp[9];
        airngin_diag_fingerprint(value, len, fp);
        ESP_LOGI(tag, "%-14s = <hidden> len=%u fp=%s", name, (unsigned)len, fp);
        return;
    }
    size_t used = 0;
    airngin_text_escape(value, len, escaped, sizeof(escaped), &used);
    if (is_secret) {   /* LOG_SECRETS: plain text AND the fingerprint, so stages can still be compared at a glance */
        char fp[9];
        airngin_diag_fingerprint(value, len, fp);
        ESP_LOGI(tag, "%-14s = \"%s\"%s len=%u fp=%s", name, escaped, used < len ? "..." : "", (unsigned)len, fp);
        return;
    }
    ESP_LOGI(tag, "%-14s = \"%s\"%s len=%u", name, escaped, used < len ? "..." : "", (unsigned)len);
}

void airngin_diag_hex(const char *tag, const char *name, const char *value, size_t len)
{
#if AG_DIAG_SECRETS && AG_DIAG_VERBOSE_ON
    char line[3 * 16 + 1];
    ESP_LOGI(tag, "%s HEX (%u bytes):", name, (unsigned)len);
    for (size_t off = 0; off < len; off += 16) {
        const size_t chunk = len - off < 16 ? len - off : 16;
        airngin_text_hex((const unsigned char *)value + off, chunk, line, sizeof(line));
        ESP_LOGI(tag, "  %s", line);
    }
#else
    (void)tag; (void)name; (void)value; (void)len;
#endif
}

void airngin_diag_dump_raw(const char *tag, const char *label, const char *data, size_t len)
{
#if AG_DIAG_SECRETS
    char line[400 * 4 + 1];
    ESP_LOGI(tag, "%s (%u bytes):", label, (unsigned)len);
    size_t off = 0;
    do {                                    /* chunked: the whole payload is shown, never truncated */
        size_t used = 0;
        airngin_text_escape(data + off, len - off > 400 ? 400 : len - off, line, sizeof(line), &used);
        ESP_LOGI(tag, "%s", line);
        off += used;
        if (used == 0) break;
    } while (off < len);
#else
    (void)data;
    ESP_LOGI(tag, "%s (%u bytes): <hidden - payload may contain credentials>", label, (unsigned)len);
#endif
}

void airngin_diag_print_buffer(const char *tag, const uint8_t *data, size_t len)
{
    if (!data && len != 0U) {
        ESP_LOGW(tag, "<null buffer with non-zero length>");
        return;
    }
    if (len == 0U) {
        ESP_LOGI(tag, "<empty>");
        return;
    }
    /* 128 source bytes expand to at most 512 characters (\\xNN per byte).
     * airngin_text_escape never splits an escape sequence or reads past len. */
    char line[513];
    size_t offset = 0U;
    while (offset < len) {
        const size_t input = len - offset > 128U ? 128U : len - offset;
        size_t consumed = 0U;
        (void)airngin_text_escape((const char *)data + offset, input, line, sizeof(line), &consumed);
        if (consumed == 0U) break;
        ESP_LOGI(tag, "%s", line);
        offset += consumed;
    }
}

void airngin_diag_secret_warning(void)
{
#if AG_DIAG_SECRETS
    ESP_LOGW(TAG, "################################################################");
    ESP_LOGW(TAG, "WARNING: AIRNGIN SECRET DIAGNOSTIC LOGGING IS ENABLED");
    ESP_LOGW(TAG, "Wi-Fi and MQTT credentials are printed in PLAIN TEXT.");
    ESP_LOGW(TAG, "DO NOT USE THIS BUILD IN PRODUCTION");
    ESP_LOGW(TAG, "################################################################");
#endif
}

void airngin_diag_snapshot(const char *where, bool in_owner_task)
{
#if AG_DIAG_VERBOSE_ON
    ESP_LOGI(TAG, "[%s] free heap=%u min free heap=%u largest block=%u", where,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    if (in_owner_task) {
        ESP_LOGI(TAG, "[%s] owner stack high-water=%u bytes never used", where,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
#else
    (void)where; (void)in_owner_task;
#endif
}

void airngin_diag_print_status(const char *tag, const char *title)
{
    airngin_status_t s = AIRNGIN_STATUS_INIT;
    if (airngin_get_status(&s) != ESP_OK) {
        ESP_LOGW(tag, "%s: status unavailable", title);
        return;
    }
    ESP_LOGI(tag, "========== AirNgin Status (%s) ==========", title);
    ESP_LOGI(tag, "Lifecycle             : %s", airngin_diag_state_name(s.lifecycle));
    ESP_LOGI(tag, "Network mode          : %s", airngin_diag_mode_name(s.network_mode));
    ESP_LOGI(tag, "Network connected     : %s", s.network_connected ? "CONNECTED" : "DISCONNECTED");
    ESP_LOGI(tag, "Clock trusted         : %s", airngin_diag_yesno(s.clock_trusted));
    ESP_LOGI(tag, "MQTT connected        : %s", airngin_diag_yesno(s.mqtt_connected));
    ESP_LOGI(tag, "MQTT ready            : %s", airngin_diag_yesno(s.mqtt_ready));
    ESP_LOGI(tag, "Ready                 : %s", airngin_diag_yesno(s.ready));
    ESP_LOGI(tag, "Credentials available : %s", airngin_diag_yesno(s.credentials_available));
    ESP_LOGI(tag, "Credentials rejected  : %s", airngin_diag_yesno(s.credentials_rejected));
    ESP_LOGI(tag, "Provisioning active   : %s", airngin_diag_yesno(s.provisioning_active));
    ESP_LOGI(tag, "RX dropped            : %u", (unsigned)s.rx_dropped);
    ESP_LOGI(tag, "TX rejected           : %u", (unsigned)s.tx_rejected);
    ESP_LOGI(tag, "TX failed             : %u", (unsigned)s.tx_failed);
    ESP_LOGI(tag, "=====================================================");
}

/* ---- state-change watcher (owner task only; logs transitions, never periodic noise) ------------- */
static airngin_status_t s_prev;
static bool s_have_prev;

void airngin_diag_status_watch_reset(void) { s_have_prev = false; }

void airngin_diag_status_watch(void)
{
    airngin_status_t s = AIRNGIN_STATUS_INIT;
    if (airngin_get_status(&s) != ESP_OK) return;
    if (!s_have_prev) {
        s_prev = s;
        s_have_prev = true;
        ESP_LOGI("AG_NET", "initial: network_connected=%s clock_trusted=%s mqtt_connected=%s mqtt_ready=%s ready=%s",
                 airngin_diag_yesno(s.network_connected), airngin_diag_yesno(s.clock_trusted),
                 airngin_diag_yesno(s.mqtt_connected), airngin_diag_yesno(s.mqtt_ready), airngin_diag_yesno(s.ready));
        return;
    }
    if (s.network_connected != s_prev.network_connected) {
        ESP_LOGI("AG_NET", "network_connected %d -> %d source=%s", (int)s_prev.network_connected,
                 (int)s.network_connected, airngin_diag_mode_name(s.network_mode));
    }
    if (s.clock_trusted != s_prev.clock_trusted) {
        ESP_LOGI("AG_TIME", "clock_trusted %d -> %d%s", (int)s_prev.clock_trusted, (int)s.clock_trusted,
                 s.clock_trusted ? "" : " (MQTT cannot start until the clock is trusted again)");
    }
    if (s.mqtt_connected != s_prev.mqtt_connected || s.mqtt_ready != s_prev.mqtt_ready) {
        ESP_LOGI("AG_MQTT", "mqtt_connected %d -> %d, mqtt_ready %d -> %d", (int)s_prev.mqtt_connected,
                 (int)s.mqtt_connected, (int)s_prev.mqtt_ready, (int)s.mqtt_ready);
    }
    if (s.provisioning_active != s_prev.provisioning_active) {
        ESP_LOGI("AG_PROV", "provisioning_active %d -> %d", (int)s_prev.provisioning_active, (int)s.provisioning_active);
    }
    if (s.credentials_rejected != s_prev.credentials_rejected) {
        ESP_LOGW("AG_MQTT", "credentials_rejected %d -> %d", (int)s_prev.credentials_rejected, (int)s.credentials_rejected);
    }
    if (s.ready != s_prev.ready) {
        ESP_LOGI("AG_LIFE", "ready %d -> %d", (int)s_prev.ready, (int)s.ready);
        ESP_LOGI("AG_LIFE", "network=%d clock=%d mqtt_connected=%d mqtt_ready=%d", (int)s.network_connected,
                 (int)s.clock_trusted, (int)s.mqtt_connected, (int)s.mqtt_ready);
        if (s.ready) {
            airngin_diag_snapshot("after MQTT_READY", true);
        } else {
            const char *why = !s.network_connected ? "NETWORK_DISCONNECTED"
                              : !s.clock_trusted ? "CLOCK_NOT_TRUSTED"
                              : !s.mqtt_connected ? "MQTT_DISCONNECTED"
                              : !s.mqtt_ready ? "MQTT_SUBSCRIPTION_LOST"
                              : s.lifecycle != AIRNGIN_STATE_RUNNING ? "LIFECYCLE" : "UNKNOWN";
            ESP_LOGI("AG_LIFE", "ready fell: reason=%s", why);
        }
    }
    s_prev = s;
}

#endif /* CONFIG_AIRNGIN_DIAGNOSTIC_LOGS */

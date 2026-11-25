#include "airngin.h"
#include "airngin_internal.h"
#include "airngin_identity.h"
#include "airngin_utf8.h"
#include "airngin_diag.h"
#include "airngin_topic.h"
#include "airngin_notify.h"
#include "airngin_database.h"
#include "esp_timer.h"
#if CONFIG_AIRNGIN_USE_AES
#include "airngin_crypto.h"
#endif
#include "airngin_version.h"
#include "AirNginConfig.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
#include <string.h>

#ifndef CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS
#define CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS 3000
#endif

#ifndef CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH
#define CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_AIRNGIN_TX_QUEUE_DEPTH
#define CONFIG_AIRNGIN_TX_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_AIRNGIN_TASK_STACK_SIZE
#define CONFIG_AIRNGIN_TASK_STACK_SIZE 16384
#endif

#define AIRNGIN_SERIAL_CAPACITY          (AIRNGIN_SERIAL_LENGTH + 1U)
#define AIRNGIN_PRODUCER_CAPACITY        (AIRNGIN_PRODUCER_LENGTH + 1U)
#define AIRNGIN_LABEL_CAPACITY           (AIRNGIN_PROVISIONING_LABEL_MAX + 1U)
#define AIRNGIN_PASSWORD_CAPACITY        (AIRNGIN_PROVISIONING_PASSWORD_MAX + 1U)

typedef struct {
    char serial[AIRNGIN_SERIAL_CAPACITY];
    char producer_code[AIRNGIN_PRODUCER_CAPACITY];
    char provisioning_label[AIRNGIN_LABEL_CAPACITY];
    char provisioning_password[AIRNGIN_PASSWORD_CAPACITY];
    airngin_command_handler_t command_handler;
    airngin_event_handler_t event_handler;
    void *user_ctx;
    airngin_network_mode_t network_mode;
    airngin_profile cloud;      /* EXTERNAL mode credentials (copied); sensitive, zeroed on deinit */
    bool has_cloud;
    uint32_t external_reconnect_delay_ms;
} airngin_runtime_config;

static portMUX_TYPE ag_lock = portMUX_INITIALIZER_UNLOCKED;
static airngin_state_t ag_state = AIRNGIN_STATE_UNINITIALIZED;
/* True while one task is inside the stop sequence; serializes stop retries. */
static bool ag_stop_busy;
static airngin_runtime_config ag_config;
/* EXTERNAL mode: host-reported connectivity. Survives stop/start, forgotten on init/deinit. */
static bool ag_ext_connected;
static uint32_t ag_ext_generation;
/* EXTERNAL mode credential bookkeeping (guarded by ag_lock). */
static uint32_t ag_cloud_generation;
static bool ag_cloud_rejected;
/* AirNgin time feed (guarded by ag_lock): the application's wish, and the latest valid packet. */
static bool ag_time_want;
static airngin_time_source_t ag_time_source;
static bool ag_time_have;
static airngin_time_info_t ag_time_latest;

static bool ag_valid_label(const char *label)
{
    if (!label) return false;
    const size_t len = strnlen(label, AIRNGIN_LABEL_CAPACITY);
    if (!len || len > AIRNGIN_PROVISIONING_LABEL_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)label[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool ag_valid_password(const char *password)
{
    if (!password) return false;
    const size_t len = strnlen(password, AIRNGIN_PASSWORD_CAPACITY);
    if (len < 8U || len > AIRNGIN_PROVISIONING_PASSWORD_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = (unsigned char)password[i];
        if (c < 32U || c > 126U) return false;
    }
    return true;
}

/* Smallest struct_size that covers every field of the given API version. */
static size_t ag_config_min_size(uint32_t api_version)
{
    /* v1 ends before cloud_credentials, v2 before external_reconnect_delay_ms, v3 is the full structure */
    if (api_version >= 3U) return sizeof(airngin_config_t);
    return api_version == 2U ? offsetof(airngin_config_t, external_reconnect_delay_ms)
                             : offsetof(airngin_config_t, cloud_credentials);
}

airngin_config_t airngin_config_default(void)
{
    const airngin_config_t config = {
        .struct_size = sizeof(airngin_config_t),
        .api_version = AIRNGIN_API_VERSION,
        .serial = CONFIG_AIRNGIN_SERIAL,
        .producer_code = CONFIG_AIRNGIN_PRODUCER,
        .network_mode = AIRNGIN_NETWORK_MANAGED_WIFI,
        .provisioning_label = "Device",
        .provisioning_password = CONFIG_AIRNGIN_PROVISIONING_PASSWORD,
        .command_handler = NULL,
        .event_handler = NULL,
        .user_ctx = NULL,
        .cloud_credentials = NULL,
        .external_reconnect_delay_ms = CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS,
    };
    return config;
}

airngin_state_t airngin_get_state(void)
{
    portENTER_CRITICAL(&ag_lock);
    const airngin_state_t state = ag_state;
    portEXIT_CRITICAL(&ag_lock);
    return state;
}

#if AG_DIAG
/* One readable configuration summary after a successful init. Only values that really exist are printed. */
static void ag_diag_log_config(void)
{
    const char *T = "AG_LIFE";
    const airngin_network_mode_t mode = airngin_internal_network_mode();
    ESP_LOGI(T, "========== AirNgin Configuration ==========");
    ESP_LOGI(T, "SDK version        : %s", AIRNGIN_VERSION_STRING);
    ESP_LOGI(T, "Network mode       : %s", airngin_diag_mode_name(mode));
    {   /* compile-time application protocol (CONFIG_AIRNGIN_USE_AES); the topics also appear when the client is created */
        char pub[64], sub[80];
        (void)airngin_topic_build(AIRNGIN_TOPIC_DEVICE_TO_SERVER, AIRNGIN_USE_AES, "<projectCode>", airngin_internal_serial(), pub, sizeof(pub));
        (void)airngin_topic_build(AIRNGIN_TOPIC_SERVER_TO_DEVICE, AIRNGIN_USE_AES, "<projectCode>", airngin_internal_serial(), sub, sizeof(sub));
        ESP_LOGI(T, "AirNgin protocol mode : %s", AIRNGIN_USE_AES ? "AES" : "PLAIN");
        ESP_LOGI(T, "AES application layer : %s", AIRNGIN_USE_AES ? "enabled (AES-256-GCM envelope on top of TLS; needs a valid encryptionKey)" : "disabled");
        ESP_LOGI(T, "Publish topic         : %s", pub);
        ESP_LOGI(T, "Subscribe topic       : %s", sub);
    }
    ESP_LOGI(T, "Device serial      : %s", airngin_internal_serial());
    ESP_LOGI(T, "Producer code      : %s", airngin_internal_producer_code());
    if (mode == AIRNGIN_NETWORK_MANAGED_WIFI) {
        ESP_LOGI(T, "Provision label    : %s", airngin_internal_provisioning_label());
        ESP_LOGI(T, "Provision AP pass  : configured, len=%u (never printed)",
                 (unsigned)strlen(airngin_internal_provisioning_password()));
        ESP_LOGI(T, "Credential source  : Managed NVS profile (read when the runtime starts)");
    } else {
        ESP_LOGI(T, "Credential source  : External config (airngin_config_t.cloud_credentials)");
        ESP_LOGI(T, "Reconnect delay    : %u ms", (unsigned)airngin_internal_external_reconnect_delay_ms());
        airngin_profile cloud;
        if (airngin_internal_cloud_profile(&cloud, NULL)) {
            ESP_LOGI(T, "Project code       : %s", cloud.projectCode);
            ESP_LOGI(T, "Broker host        : %s", cloud.mqttBroker);
            ESP_LOGI(T, "Client ID          : %s", cloud.clientId);
            airngin_diag_field(T, "Username", cloud.mqttUser, strlen(cloud.mqttUser), true);
            airngin_diag_field(T, "Password", cloud.mqttPass, strlen(cloud.mqttPass), true);
#if CONFIG_AIRNGIN_USE_AES
            {
                char fp[9];
                airngin_diag_fingerprint(cloud.aesKey, strnlen(cloud.aesKey, sizeof(cloud.aesKey)), fp);
                ESP_LOGI(T, "AES key            : source=external config, length=%u bytes, fp=%s (never printed)", (unsigned)AIRNGIN_AES_KEY_LEN, fp);
            }
#endif
            airngin_secure_zero(&cloud, sizeof(cloud));
        }
    }
    ESP_LOGI(T, "Task stack         : %u bytes", (unsigned)CONFIG_AIRNGIN_TASK_STACK_SIZE);
    ESP_LOGI(T, "RX queue depth     : %u", (unsigned)CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH);
    ESP_LOGI(T, "TX queue depth     : %u", (unsigned)CONFIG_AIRNGIN_TX_QUEUE_DEPTH);
    ESP_LOGI(T, "MQTT port          : %u", (unsigned)AIRNGIN_MQTT_PORT);
    ESP_LOGI(T, "TLS                : enabled (certificate + hostname verification)");
    ESP_LOGI(T, "Root CA            : embedded");
    ESP_LOGI(T, "Diagnostics        : enabled%s%s", AG_DIAG_VERBOSE_ON ? ", verbose" : "",
             AG_DIAG_SECRETS ? ", SECRET LOGGING" : "");
    ESP_LOGI(T, "============================================");
}
#endif

esp_err_t airngin_init(const airngin_config_t *config)
{
    airngin_config_t full;                      /* always the newest layout; older callers leave the tail zero */
    if (!config) {
        full = airngin_config_default();
    } else {
        if (config->api_version == 0U || config->api_version > AIRNGIN_API_VERSION) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (config->struct_size < ag_config_min_size(config->api_version)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memset(&full, 0, sizeof(full));
        memcpy(&full, config, config->struct_size < sizeof(full) ? config->struct_size : sizeof(full));
        if (config->struct_size <= offsetof(airngin_config_t, external_reconnect_delay_ms)) {
            full.external_reconnect_delay_ms = CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS;   /* older config: default */
        }
    }
    config = &full;

    if (config->network_mode != AIRNGIN_NETWORK_MANAGED_WIFI &&
        config->network_mode != AIRNGIN_NETWORK_EXTERNAL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
    if (config->network_mode == AIRNGIN_NETWORK_MANAGED_WIFI) return ESP_ERR_NOT_SUPPORTED;
#endif
    if (!config->serial || !config->producer_code ||
        !airngin_identity_valid(config->serial, config->producer_code)) {
        return ESP_ERR_INVALID_ARG;
    }

    airngin_runtime_config next;
    memset(&next, 0, sizeof(next));
    next.network_mode = config->network_mode;
    if (config->network_mode == AIRNGIN_NETWORK_MANAGED_WIFI) {
        /* Contradictory configuration is a programming error: refuse rather than ignore. */
        if (config->cloud_credentials) return ESP_ERR_INVALID_ARG;
        if (!ag_valid_label(config->provisioning_label) ||
            !ag_valid_password(config->provisioning_password)) {
            return ESP_ERR_INVALID_ARG;
        }
        const size_t label_len = strlen(config->provisioning_label);
        memcpy(next.provisioning_label, config->provisioning_label, label_len + 1U);
        const size_t password_len = strlen(config->provisioning_password);
        memcpy(next.provisioning_password, config->provisioning_password, password_len + 1U);
    } else {
        if (!config->cloud_credentials ||
            !airngin_cloud_to_profile(config->cloud_credentials, &next.cloud)) {
            airngin_secure_zero(&next, sizeof(next));
            return ESP_ERR_INVALID_ARG;
        }
        if (config->external_reconnect_delay_ms > AIRNGIN_EXTERNAL_RECONNECT_DELAY_MAX_MS) {
            airngin_secure_zero(&next, sizeof(next));
            return ESP_ERR_INVALID_ARG;
        }
        next.has_cloud = true;
    }
    next.external_reconnect_delay_ms = config->external_reconnect_delay_ms;
    memcpy(next.serial, config->serial, AIRNGIN_SERIAL_LENGTH);
    next.serial[AIRNGIN_SERIAL_LENGTH] = '\0';
    memcpy(next.producer_code, config->producer_code, AIRNGIN_PRODUCER_LENGTH);
    next.producer_code[AIRNGIN_PRODUCER_LENGTH] = '\0';
    next.command_handler = config->command_handler;
    next.event_handler = config->event_handler;
    next.user_ctx = config->user_ctx;

    portENTER_CRITICAL(&ag_lock);
    if (ag_state != AIRNGIN_STATE_UNINITIALIZED) {
        portEXIT_CRITICAL(&ag_lock);
        airngin_secure_zero(&next, sizeof(next));
        AG_DIAG_W("AG_LIFE", "init rejected: already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    ag_config = next;
    ag_ext_connected = false;
    ag_ext_generation = 0;
    ag_cloud_generation = 1;
    ag_cloud_rejected = false;
    ag_time_want = false;
    ag_time_have = false;
    ag_state = AIRNGIN_STATE_INITIALIZED;
    portEXIT_CRITICAL(&ag_lock);
    airngin_secure_zero(&next, sizeof(next));
    AG_DIAG_I("AG_LIFE", "state UNINITIALIZED -> INITIALIZED");
    airngin_diag_secret_warning();
    airngin_database_request_log_warning();
#if AG_DIAG
    ag_diag_log_config();
#endif
    return ESP_OK;
}

esp_err_t airngin_start(void)
{
    portENTER_CRITICAL(&ag_lock);
    if (ag_state != AIRNGIN_STATE_INITIALIZED) {
        AG_DIAG_DECL(const airngin_state_t seen = ag_state)
        portEXIT_CRITICAL(&ag_lock);
        AG_DIAG_W("AG_LIFE", "start rejected state=%s (needs INITIALIZED)", airngin_diag_state_name(seen));
        return ESP_ERR_INVALID_STATE;
    }
    ag_state = AIRNGIN_STATE_STARTING;
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_LIFE", "state INITIALIZED -> STARTING");
    airngin_diag_status_watch_reset();

    const esp_err_t err = airngin_runtime_start();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&ag_lock);
        ag_state = AIRNGIN_STATE_INITIALIZED;
        portEXIT_CRITICAL(&ag_lock);
        AG_DIAG_E("AG_LIFE", "state STARTING -> INITIALIZED: runtime start FAILED " AG_ERRF, AG_ERRV(err));
        return err;
    }

    /*
     * The owner task exists but is gated: deliver STARTED first, then let it
     * run. The state stays STARTING until the owner is released so that no
     * other task can stop the runtime underneath this release.
     */
    airngin_internal_emit_event(AIRNGIN_EVENT_STARTED);
    airngin_runtime_release_owner();

    portENTER_CRITICAL(&ag_lock);
    ag_state = AIRNGIN_STATE_RUNNING;
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_LIFE", "state STARTING -> RUNNING (owner task released)");
    airngin_diag_snapshot("after start", false);
    return ESP_OK;
}

esp_err_t airngin_stop(void)
{
    /* Waiting for the owner task from the owner task would wait for itself. */
    if (airngin_runtime_in_owner_task()) {
        AG_DIAG_W("AG_LIFE", "stop rejected: called from the AirNgin owner task (would wait for itself)");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&ag_lock);
    if (ag_stop_busy ||
        (ag_state != AIRNGIN_STATE_RUNNING && ag_state != AIRNGIN_STATE_STOPPING)) {
        AG_DIAG_DECL(const airngin_state_t seen = ag_state)
        AG_DIAG_DECL(const bool busy = ag_stop_busy)
        portEXIT_CRITICAL(&ag_lock);
        AG_DIAG_W("AG_LIFE", "stop rejected state=%s%s", airngin_diag_state_name(seen), busy ? " (another stop in progress)" : "");
        return ESP_ERR_INVALID_STATE;
    }
    AG_DIAG_DECL(const airngin_state_t before = ag_state)
    ag_state = AIRNGIN_STATE_STOPPING;
    ag_stop_busy = true;
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_LIFE", "%s", before == AIRNGIN_STATE_RUNNING ? "state RUNNING -> STOPPING"
                                                              : "state STOPPING: stop retry (previous attempt was incomplete)");

    const esp_err_t err = airngin_runtime_stop();

    airngin_event_handler_t handler = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&ag_lock);
    ag_stop_busy = false;
    if (err == ESP_OK) {
        handler = ag_config.event_handler;
        ctx = ag_config.user_ctx;
        ag_state = AIRNGIN_STATE_INITIALIZED;
    } /* else: remain STOPPING, resources may still be live, caller may retry */
    portEXIT_CRITICAL(&ag_lock);

    if (err == ESP_OK) {
        AG_DIAG_I("AG_LIFE", "state STOPPING -> INITIALIZED");
        airngin_diag_snapshot("after stop", false);
    } else {
        AG_DIAG_W("AG_LIFE", "stop incomplete " AG_ERRF "; state stays STOPPING, call airngin_stop() again", AG_ERRV(err));
    }
    if (err == ESP_OK && handler) {
        const airngin_event_data_t stopped = { .type = AIRNGIN_EVENT_STOPPED, .error = ESP_OK };
        handler(&stopped, ctx);
    }
    return err;
}

esp_err_t airngin_deinit(void)
{
    if (airngin_runtime_in_owner_task()) {
        AG_DIAG_W("AG_LIFE", "deinit rejected: called from the AirNgin owner task");
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&ag_lock);
    if (ag_state != AIRNGIN_STATE_INITIALIZED) {
        AG_DIAG_DECL(const airngin_state_t seen = ag_state)
        portEXIT_CRITICAL(&ag_lock);
        AG_DIAG_W("AG_LIFE", "deinit rejected state=%s (needs INITIALIZED)", airngin_diag_state_name(seen));
        return ESP_ERR_INVALID_STATE;
    }
    airngin_secure_zero(&ag_config, sizeof(ag_config));   /* includes the copied cloud credentials */
    ag_ext_connected = false;
    ag_ext_generation = 0;
    ag_cloud_generation = 0;
    ag_cloud_rejected = false;
    ag_time_want = false;
    ag_time_have = false;
    memset(&ag_time_latest, 0, sizeof(ag_time_latest));
    ag_state = AIRNGIN_STATE_UNINITIALIZED;
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_LIFE", "state INITIALIZED -> UNINITIALIZED (configuration and credentials wiped)");
    return ESP_OK;
}

esp_err_t airngin_get_status(airngin_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    if (status->struct_size < sizeof(*status)) return ESP_ERR_INVALID_SIZE;

    airngin_runtime_status_t rt;
    memset(&rt, 0, sizeof(rt));
    const airngin_state_t lifecycle = airngin_get_state();
    airngin_runtime_get_status(&rt);

    memset(status, 0, sizeof(*status));
    status->struct_size = sizeof(*status);
    status->lifecycle = lifecycle;
    status->network_mode = airngin_internal_network_mode();
    status->network_connected = rt.network_connected;
    status->mqtt_connected = rt.mqtt_connected;
    status->mqtt_ready = rt.mqtt_ready && rt.mqtt_connected;
    status->credentials_available = rt.credentials_available;
    status->provisioning_active = rt.provisioning_active;
    status->clock_trusted = rt.clock_trusted;
    status->rx_dropped = rt.rx_dropped;
    status->tx_rejected = rt.tx_rejected;
    status->tx_failed = rt.tx_failed;
    status->credentials_rejected = rt.credentials_rejected;
    status->ready = lifecycle == AIRNGIN_STATE_RUNNING && status->network_connected &&
                    status->clock_trusted && status->mqtt_ready;
    return ESP_OK;
}

bool airngin_is_ready(void)
{
    airngin_status_t status = AIRNGIN_STATUS_INIT;
    return airngin_get_status(&status) == ESP_OK && status.ready;
}

static esp_err_t ag_publish_impl(const char *key, const char *value);

esp_err_t airngin_publish(const char *key, const char *value)
{
    const esp_err_t err = ag_publish_impl(key, value);
#if AG_DIAG
    if (key && value) {
        const size_t vlen = strnlen(value, AIRNGIN_COMMAND_VALUE_MAX + 1U);
        AG_DIAG_I("AG_TX", "publish requested key=\"%.64s\" value_len=%u", key, (unsigned)vlen);
        AG_DIAG_V("AG_TX", "publish value=\"%.96s\"%s", value, vlen > 96 ? "..." : "");
    }
    if (err == ESP_OK) AG_DIAG_I("AG_TX", "accepted into outgoing queue");
    else AG_DIAG_W("AG_TX", "publish NOT accepted: " AG_ERRF "%s", AG_ERRV(err),
                   err == ESP_ERR_INVALID_STATE ? " (not RUNNING/ready: no offline buffering)" :
                   err == ESP_ERR_NO_MEM ? " (outgoing queue full)" : "");
#endif
    return err;
}

static esp_err_t ag_publish_impl(const char *key, const char *value)
{
    if (!key || !value) return ESP_ERR_INVALID_ARG;
    const size_t key_len = strnlen(key, AIRNGIN_COMMAND_KEY_MAX + 1U);
    const size_t value_len = strnlen(value, AIRNGIN_COMMAND_VALUE_MAX + 1U);
    if (key_len == 0U || key_len > AIRNGIN_COMMAND_KEY_MAX ||
        value_len > AIRNGIN_COMMAND_VALUE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!airngin_utf8_valid((const unsigned char *)key, key_len) ||
        !airngin_utf8_valid((const unsigned char *)value, value_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (airngin_get_state() != AIRNGIN_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    return airngin_runtime_publish(key, key_len, value, value_len);
}

esp_err_t airngin_report(const char *key, const char *value)
{
    /* Same wire behaviour today; kept separate so the protocol can distinguish them later. */
    return airngin_publish(key, value);
}

/* ---- AirNgin time feed ------------------------------------------------------------------------------------- */
esp_err_t airngin_time_subscribe(airngin_time_source_t source)
{
    if (source != AIRNGIN_TIME_SOURCE_UTC && source != AIRNGIN_TIME_SOURCE_TEHRAN) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&ag_lock);
    const bool allowed = ag_state != AIRNGIN_STATE_UNINITIALIZED && ag_state != AIRNGIN_STATE_STOPPING;
    if (allowed) {
        ag_time_want = true;
        ag_time_source = source;
    }
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_TIME_MQTT", "time subscription requested: %s (%s)", source == AIRNGIN_TIME_SOURCE_TEHRAN ? "Tehran" : "UTC",
              allowed ? "recorded; the owner task subscribes when MQTT is ready" : "REFUSED: lifecycle state");
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t airngin_time_unsubscribe(void)
{
    portENTER_CRITICAL(&ag_lock);
    const bool allowed = ag_state != AIRNGIN_STATE_UNINITIALIZED && ag_state != AIRNGIN_STATE_STOPPING;
    if (allowed) ag_time_want = false;
    portEXIT_CRITICAL(&ag_lock);
    AG_DIAG_I("AG_TIME_MQTT", "time unsubscription requested (%s)", allowed ? "recorded" : "REFUSED: lifecycle state");
    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t airngin_time_get_latest(airngin_time_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err;
    portENTER_CRITICAL(&ag_lock);
    if (ag_state == AIRNGIN_STATE_UNINITIALIZED) err = ESP_ERR_INVALID_STATE;
    else if (!ag_time_have) err = ESP_ERR_NOT_FOUND;
    else { *out = ag_time_latest; err = ESP_OK; }
    portEXIT_CRITICAL(&ag_lock);
    return err;
}

bool airngin_internal_time_wanted(airngin_time_source_t *source)
{
    portENTER_CRITICAL(&ag_lock);
    const bool want = ag_time_want;
    if (source) *source = ag_time_source;
    portEXIT_CRITICAL(&ag_lock);
    return want;
}

void airngin_internal_time_store(const airngin_time_info_t *info)
{
    portENTER_CRITICAL(&ag_lock);
    ag_time_latest = *info;
    ag_time_have = true;
    portEXIT_CRITICAL(&ag_lock);
}

/* ---- notifications ---------------------------------------------------------------------------------------- */
static esp_err_t ag_notify(bool to_user, const char *code, int channel)
{
    size_t length = 0;
    esp_err_t err = airngin_notify_validate_code(code, &length);
    if (err == ESP_OK && to_user && !airngin_notify_channel_valid(channel)) err = ESP_ERR_INVALID_ARG;
#if CONFIG_AIRNGIN_USE_AES && !CONFIG_AIRNGIN_NOTIFICATION_AES_CONFIRMED
    /* Never publish a plaintext notification from an AES firmware, and the /AES topics are not confirmed yet. */
    if (err == ESP_OK) err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err == ESP_OK && airngin_get_state() != AIRNGIN_STATE_RUNNING) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = airngin_runtime_notify(to_user, channel, code, length);
    AG_DIAG_I("AG_NOTIFY", "%s notification requested (%s%s)", to_user ? "user" : "center",
              to_user ? (channel == AIRNGIN_NOTIFICATION_SMS ? "channel=SMS (20)" : channel == AIRNGIN_NOTIFICATION_PUSH ? "channel=PUSH (30)" : "channel=INVALID") : "to the project center",
              "");
    AG_DIAG_I("AG_NOTIFY", "result: " AG_ERRF "%s", AG_ERRV(err),
              err == ESP_OK ? " (queued; ESP_OK is NOT a delivery confirmation)" :
              err == ESP_ERR_INVALID_STATE ? " (not RUNNING / MQTT not ready; nothing is stored)" :
              err == ESP_ERR_NOT_SUPPORTED ? " (AES build: /AES notification topics not confirmed)" : "");
    return err;
}

esp_err_t airngin_send_notification_to_center(const char *code) { return ag_notify(false, code, 0); }

esp_err_t airngin_send_notification_to_user(const char *code, airngin_notification_channel_t channel)
{
    return ag_notify(true, code, (int)channel);
}

esp_err_t airngin_send_sms_to_center(const char *code) { return airngin_send_notification_to_center(code); }
esp_err_t airngin_send_sms_to_user(const char *code) { return airngin_send_notification_to_user(code, AIRNGIN_NOTIFICATION_SMS); }
esp_err_t airngin_send_push_to_user(const char *code) { return airngin_send_notification_to_user(code, AIRNGIN_NOTIFICATION_PUSH); }

esp_err_t airngin_cancel_provisioning(void)
{
    if (airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL) return ESP_ERR_NOT_SUPPORTED;
    const esp_err_t err = airngin_get_state() == AIRNGIN_STATE_RUNNING ? airngin_runtime_cancel_provisioning()
                                                                      : ESP_ERR_INVALID_STATE;
    AG_DIAG_I("AG_PROV", "airngin_cancel_provisioning: " AG_ERRF "%s", AG_ERRV(err),
              err == ESP_OK ? " (owner task will close the panel)" :
              err == ESP_ERR_INVALID_STATE ? " (nothing active to cancel, or a configuration was already committed)" : "");
    return err;
}

esp_err_t airngin_request_provisioning(void)
{
    if (airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL) return ESP_ERR_NOT_SUPPORTED;
    if (airngin_get_state() != AIRNGIN_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    return airngin_runtime_request_provisioning();
}

esp_err_t airngin_command_result_set(airngin_command_result_t *result, const char *value)
{
    if (!result || !value) return ESP_ERR_INVALID_ARG;
    const size_t len = strnlen(value, AIRNGIN_COMMAND_VALUE_MAX + 1U);
    if (len > AIRNGIN_COMMAND_VALUE_MAX) return ESP_ERR_INVALID_SIZE;
    memcpy(result->value, value, len + 1U);
    result->has_value = true;
    return ESP_OK;
}

esp_err_t airngin_command_result_set_key(airngin_command_result_t *result, const char *key)
{
    if (!result || !key) return ESP_ERR_INVALID_ARG;
    const size_t len = strnlen(key, AIRNGIN_COMMAND_KEY_MAX + 1U);
    if (len == 0U || len > AIRNGIN_COMMAND_KEY_MAX) return ESP_ERR_INVALID_SIZE;
    memcpy(result->key, key, len + 1U);
    result->has_key = true;
    return ESP_OK;
}

esp_err_t airngin_external_network_set_connected(bool connected)
{
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&ag_lock);
    if (ag_state == AIRNGIN_STATE_UNINITIALIZED) {
        err = ESP_ERR_INVALID_STATE;
    } else if (ag_config.network_mode != AIRNGIN_NETWORK_EXTERNAL) {
        err = ESP_ERR_NOT_SUPPORTED;
    } else if (ag_ext_connected != connected) {
        ag_ext_connected = connected;
        ag_ext_generation++;       /* every real edge is visible to the owner, even a quick down/up pair */
    }
    portEXIT_CRITICAL(&ag_lock);
#if AG_DIAG
    if (err == ESP_OK) AG_DIAG_I("AG_EXT", "host reports network %s (edge counter now %u)", connected ? "UP" : "DOWN",
                                 (unsigned)ag_ext_generation);
    else AG_DIAG_W("AG_EXT", "set_connected(%s) refused: " AG_ERRF, connected ? "true" : "false", AG_ERRV(err));
#endif
    return err;
}

airngin_network_mode_t airngin_internal_network_mode(void)
{
    portENTER_CRITICAL(&ag_lock);
    const airngin_network_mode_t mode = ag_state == AIRNGIN_STATE_UNINITIALIZED
                                            ? AIRNGIN_NETWORK_MANAGED_WIFI : ag_config.network_mode;
    portEXIT_CRITICAL(&ag_lock);
    return mode;
}

bool airngin_internal_cloud_profile(airngin_profile *out, uint32_t *generation)
{
    if (!out) return false;
    bool ok = false;
    portENTER_CRITICAL(&ag_lock);
    if (ag_state != AIRNGIN_STATE_UNINITIALIZED && ag_config.has_cloud) {
        *out = ag_config.cloud;
        if (generation) *generation = ag_cloud_generation;   /* credentials and generation are read together */
        ok = true;
    }
    portEXIT_CRITICAL(&ag_lock);
    return ok;
}

uint32_t airngin_internal_cloud_generation(void)
{
    portENTER_CRITICAL(&ag_lock);
    const uint32_t generation = ag_cloud_generation;
    portEXIT_CRITICAL(&ag_lock);
    return generation;
}

uint32_t airngin_internal_external_reconnect_delay_ms(void)
{
    portENTER_CRITICAL(&ag_lock);
    const uint32_t delay = ag_config.external_reconnect_delay_ms;
    portEXIT_CRITICAL(&ag_lock);
    return delay;
}

void airngin_internal_credentials_rejected(uint32_t generation)
{
    portENTER_CRITICAL(&ag_lock);
    if (generation == ag_cloud_generation) ag_cloud_rejected = true;   /* stale verdicts are ignored */
    portEXIT_CRITICAL(&ag_lock);
}

void airngin_internal_credentials_accepted(uint32_t generation)
{
    portENTER_CRITICAL(&ag_lock);
    if (generation == ag_cloud_generation) ag_cloud_rejected = false;
    portEXIT_CRITICAL(&ag_lock);
}

bool airngin_internal_credentials_rejected_flag(void)
{
    portENTER_CRITICAL(&ag_lock);
    const bool rejected = ag_cloud_rejected;
    portEXIT_CRITICAL(&ag_lock);
    return rejected;
}

esp_err_t airngin_external_set_cloud_credentials(const airngin_cloud_credentials_t *credentials)
{
    /* Cheap lifecycle/mode gate first so the error is deterministic for managed callers. */
    portENTER_CRITICAL(&ag_lock);
    const airngin_state_t state = ag_state;
    const airngin_network_mode_t mode = ag_config.network_mode;
    portEXIT_CRITICAL(&ag_lock);
    if (state == AIRNGIN_STATE_UNINITIALIZED || state == AIRNGIN_STATE_STOPPING) return ESP_ERR_INVALID_STATE;
    if (mode != AIRNGIN_NETWORK_EXTERNAL) return ESP_ERR_NOT_SUPPORTED;

    airngin_profile fresh;
    if (!credentials || !airngin_cloud_to_profile(credentials, &fresh)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_OK;
    portENTER_CRITICAL(&ag_lock);
    if (ag_state == AIRNGIN_STATE_UNINITIALIZED || ag_state == AIRNGIN_STATE_STOPPING) {
        err = ESP_ERR_INVALID_STATE;            /* lifecycle changed while we validated */
    } else {
        airngin_secure_zero(&ag_config.cloud, sizeof(ag_config.cloud));   /* wipe the previous secrets first */
        ag_config.cloud = fresh;
        ag_config.has_cloud = true;
        ag_cloud_generation++;
        ag_cloud_rejected = false;
    }
    portEXIT_CRITICAL(&ag_lock);
#if AG_DIAG
    if (err == ESP_OK) {
        AG_DIAG_I("AG_EXT", "cloud credentials replaced -> generation %u", (unsigned)airngin_internal_cloud_generation());
        AG_DIAG_I("AG_EXT", "project=%s broker=%s client_id=%s", fresh.projectCode, fresh.mqttBroker, fresh.clientId);
        airngin_diag_field("AG_EXT", "username", fresh.mqttUser, strlen(fresh.mqttUser), true);
        airngin_diag_field("AG_EXT", "password", fresh.mqttPass, strlen(fresh.mqttPass), true);
    } else {
        AG_DIAG_W("AG_EXT", "cloud credential replacement refused: " AG_ERRF, AG_ERRV(err));
    }
#endif
    airngin_secure_zero(&fresh, sizeof(fresh));
    return err;
}

void airngin_internal_external_network(bool *connected, uint32_t *generation)
{
    portENTER_CRITICAL(&ag_lock);
    if (connected) *connected = ag_ext_connected;
    if (generation) *generation = ag_ext_generation;
    portEXIT_CRITICAL(&ag_lock);
}

const char *airngin_internal_serial(void)
{
    return ag_config.serial;
}

const char *airngin_internal_producer_code(void)
{
    return ag_config.producer_code;
}

const char *airngin_internal_provisioning_label(void)
{
    return ag_config.provisioning_label;
}

const char *airngin_internal_provisioning_password(void)
{
    return ag_config.provisioning_password;
}

esp_err_t airngin_internal_dispatch(const airngin_command_t *command,
                                    airngin_command_result_t *result)
{
    if (!command || !result) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&ag_lock);
    const airngin_command_handler_t handler = ag_config.command_handler;
    void *const ctx = ag_config.user_ctx;
    portEXIT_CRITICAL(&ag_lock);
    if (!handler) {
        AG_DIAG_W("AG_CMD", "no command handler registered: key=\"%.64s\" ignored", command->key);
        return ESP_ERR_NOT_SUPPORTED;
    }
    memset(result, 0, sizeof(*result));
    AG_DIAG_I("AG_CMD", "dispatch key=\"%.64s\" value=\"%.64s\"%s", command->key, command->value,
              strlen(command->value) > 64 ? "..." : "");
    const esp_err_t handler_err = handler(command, result, ctx);
    AG_DIAG_I("AG_CMD", "handler result=" AG_ERRF, AG_ERRV(handler_err));
    if (handler_err == ESP_OK) {
        if (result->has_value) AG_DIAG_I("AG_CMD", "observed result=\"%.64s\"%s", result->value, strlen(result->value) > 64 ? "..." : "");
        else AG_DIAG_I("AG_CMD", "handler produced no response value");
    }
    return handler_err;
}

void airngin_internal_emit(const airngin_event_data_t *event)
{
#if AG_DIAG
    if (event) {
        AG_DIAG_I("AG_LIFE", "event %s%s", airngin_diag_event_name(event->type),
                  event->error != ESP_OK ? " (with error)" : "");
        if (event->error != ESP_OK) AG_DIAG_W("AG_LIFE", "event error " AG_ERRF, AG_ERRV(event->error));
    }
#endif
    portENTER_CRITICAL(&ag_lock);
    const airngin_event_handler_t handler = ag_config.event_handler;
    void *const ctx = ag_config.user_ctx;
    portEXIT_CRITICAL(&ag_lock);
    if (handler && event) handler(event, ctx);
}

void airngin_internal_emit_event(airngin_event_t type)
{
    const airngin_event_data_t event = { .type = type, .error = ESP_OK };
    airngin_internal_emit(&event);
}

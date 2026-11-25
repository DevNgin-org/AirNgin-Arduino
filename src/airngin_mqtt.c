#include "airngin_transport.h"
#include "airngin_protocol.h"
#include "airngin_internal.h"
#include "airngin_clock.h"
#include "airngin_diag.h"
#include "airngin_diag_text.h"
#include "airngin_topic.h"
#include "airngin_time.h"
#include "airngin_notify.h"
#include "airngin_database.h"
#include "airngin.h"
#include "AirNginConfig.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if CONFIG_AIRNGIN_USE_AES
#include "airngin_crypto.h"
#include "esp_random.h"
#include <time.h>
#endif

/* Compile-time protocol mode: PLAIN (default) or AES. TLS (port 8883) is used in BOTH; AES is an additional
 * application layer. Topics are produced only by airngin_topic_build(). */
#if CONFIG_AIRNGIN_USE_AES
#define MT_USE_AES 1
#define MT_TOPIC_SUFFIX (sizeof(AIRNGIN_TOPIC_AES_SUFFIX) - 1U)
#else
#define MT_USE_AES 0
#define MT_TOPIC_SUFFIX 0U
#endif

#include "airngin_mqtt_root_ca.h"
#define airngin_root_ca_end airngin_root_ca_start_end

#ifndef CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH
#define CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_AIRNGIN_TX_QUEUE_DEPTH
#define CONFIG_AIRNGIN_TX_QUEUE_DEPTH 8
#endif
#ifndef CONFIG_AIRNGIN_STACK_DIAGNOSTICS
#define CONFIG_AIRNGIN_STACK_DIAGNOSTICS 0
#endif

#define MT_BROKER_PORT AIRNGIN_MQTT_PORT
#define MT_BROKER_TRANSPORT MQTT_TRANSPORT_OVER_SSL
#define MT_PROJECT_CODE_MAX (sizeof(((airngin_profile *)0)->projectCode) - 1U)
#define MT_IN_TOPIC_CAPACITY (MT_PROJECT_CODE_MAX + (sizeof("/ServerToDevice/") - 1U) + AIRNGIN_SERIAL_LENGTH + MT_TOPIC_SUFFIX + 1U)
#define MT_OUT_TOPIC_CAPACITY (MT_PROJECT_CODE_MAX + (sizeof("/DeviceToServer") - 1U) + MT_TOPIC_SUFFIX + 1U)
#define MT_NOTIFY_TOPIC_CAPACITY (MT_PROJECT_CODE_MAX + AIRNGIN_TOPIC_NAME_MAX + MT_TOPIC_SUFFIX + 1U)
#define MT_TIME_TOPIC_CAPACITY (MT_PROJECT_CODE_MAX + (sizeof("/Time/Tehran") - 1U) + 1U)
#define MT_RX_TOPIC_CAPACITY 256U
#define MT_TIME_ACK_TIMEOUT_US 10000000LL
#define MT_TIME_RETRY_US 60000000LL
/* Consecutive failed esp_mqtt_client_stop() calls after which tick stops retrying. */
#define MT_STOP_RETRY_MAX 3U
#define MT_DROP_LOG_INTERVAL_US 1000000LL

static const char *TAG = "airngin_mqtt";

/* kind of an inbound message: the personal command topic, or one of the two public time topics */
enum { MT_RX_COMMAND = 0, MT_RX_TIME_UTC = 1, MT_RX_TIME_TEHRAN = 2, MT_RX_UNKNOWN = 3, MT_RX_NONE = 0xFF };
typedef struct {
    unsigned n;
#if AG_DIAG
    uint16_t topic_len;
    char topic[MT_RX_TOPIC_CAPACITY];
#endif
    uint8_t kind;
    char text[4097];
} mt_message;

static esp_mqtt_client_handle_t mt_client;
static QueueHandle_t mt_queue;
static portMUX_TYPE mt_lock = portMUX_INITIALIZER_UNLOCKED;
/* mt_started: a connection attempt/session is in progress.
 * mt_running: esp_mqtt_client_start() succeeded and the client task is live. */
static bool mt_started, mt_running, mt_connected, mt_ready, mt_failed, mt_rejected;
static bool mt_notice_ready, mt_notice_disconnected, mt_notice_rejected;
static int mt_sub = -1;
static int64_t mt_deadline, mt_next;
static char mt_in[MT_IN_TOPIC_CAPACITY], mt_out[MT_OUT_TOPIC_CAPACITY];
static char mt_notify_center[MT_NOTIFY_TOPIC_CAPACITY], mt_notify_user[MT_NOTIFY_TOPIC_CAPACITY];
static char mt_time_topic[2][MT_TIME_TOPIC_CAPACITY];   /* [AIRNGIN_TIME_SOURCE_UTC], [AIRNGIN_TIME_SOURCE_TEHRAN] */
/*
 * AirNgin time feed subscription, tracked SEPARATELY from the core command subscription (mt_sub / mt_ready):
 * its SUBACK never makes the SDK ready and a refused/lost time subscription never makes it unavailable.
 * Session-specific (reset on every CONNECTED and on stop); the application's wish lives in airngin.c.
 * All fields are guarded by mt_lock; the owner task drives the transitions, the MQTT callback only records acks.
 */
typedef enum { MT_TIME_NONE = 0, MT_TIME_SUBSCRIBING, MT_TIME_SUBSCRIBED, MT_TIME_UNSUBSCRIBING } mt_time_state_t;
static mt_time_state_t mt_time_state;
static int mt_time_active = -1;        /* airngin_time_source_t this session subscribed (or is switching away from), -1 none */
static int mt_time_msg = -1;
static int64_t mt_time_deadline, mt_time_retry;
static size_t mt_in_len;
static char mt_broker[101];
/* The single effective cloud identity. ESP-MQTT is created from this stable copy and all internal consumers snapshot
 * this same object; it is wiped whenever the client identity ceases to exist. */
static airngin_profile mt_effective_profile;
static bool mt_effective_profile_valid;
static unsigned mt_stop_failures;
static bool mt_start_gate;   /* provisioning gate: no new MQTT session while set (guarded by mt_lock) */

/*
 * Outbound (public publish/report) queue. Items are POINTERS to exact-size heap blocks, so a slot costs
 * sizeof(void *) rather than a worst-case message. Ownership: the producer owns a block until
 * xQueueSend() succeeds (then the queue owns it; on failure the producer frees it); the owner task
 * (or the draining thread) owns it after xQueueReceive() and always frees it.
 * mt_tx_accepting/mt_tx_inflight let destroy wait out producers that already passed the ready check.
 * mt_epoch changes whenever a session ends, so a block queued by a racing producer for an old session
 * is discarded instead of being sent after a reconnect.
 */
typedef enum { MT_TX_REPORT = 0, MT_TX_NOTIFY_CENTER, MT_TX_NOTIFY_USER } mt_tx_kind;
typedef struct {
    uint32_t epoch;
    uint16_t key_len;       /* report key, or the notification message code */
    uint16_t value_len;     /* report value; 0 for notifications */
    uint8_t kind;           /* mt_tx_kind */
    uint8_t channel;        /* notification channel (20/30) */
    char data[];   /* key '\0' value '\0' */
} mt_tx;
static QueueHandle_t mt_tx_queue;
static bool mt_tx_accepting;
static unsigned mt_tx_inflight;
static uint32_t mt_epoch;
static uint32_t mt_tx_rejected, mt_tx_failed, mt_tx_discarded;
static int mt_last_reason;
static uint32_t mt_dropped;
static int64_t mt_drop_logged_us;

/* Only the serialized MQTT event callback owns these assembly fields. */
static mt_message mt_fragment;
static unsigned mt_total, mt_used;
static bool mt_assembling;

/*
 * Owner-task-only work buffers. They are static (not automatic) so the ~8 KB
 * they need never lands on the owner task's stack. Only airngin_transport_tick()
 * -> mt_dispatch() touch them, and that runs exclusively on the owner task.
 */
static mt_message mt_rx;
static char mt_response[AIRNGIN_PROTOCOL_RESPONSE_MAX + 1U];

#if CONFIG_AIRNGIN_USE_AES
/*
 * AES mode working memory (static: the owner task stack never carries it). Owner task only, like mt_rx/mt_response.
 *   mt_aes_plain    decrypted inbound message (<= 3072 bytes fit inside a 4096-byte envelope)  3073 B
 *   mt_aes_envelope outbound envelope for the largest plaintext (4096 B -> Base64 + fixed part)  5638 B
 * The key lives only here (decoded from the profile at client creation) and is wiped on destroy/replace.
 */
static uint8_t mt_aes_key[AIRNGIN_AES_KEY_LEN];
static airngin_aes_replay_t mt_aes_replay;
static uint8_t mt_aes_plain[AIRNGIN_AES_PLAIN_IN_MAX + 1U];
static char mt_aes_envelope[AIRNGIN_AES_ENVELOPE_CAP(AIRNGIN_PROTOCOL_RESPONSE_MAX)];
static uint32_t mt_aes_rejected;   /* inbound envelopes refused (never reached the command handler) */
static void mt_aes_wipe(void)
{
    airngin_secure_zero(mt_aes_key, sizeof(mt_aes_key));
    airngin_aes_replay_reset(&mt_aes_replay);
    airngin_secure_zero(mt_aes_plain, sizeof(mt_aes_plain));
    airngin_secure_zero(mt_response, sizeof(mt_response));
}
#endif

/*
 * The single outbound publish site for command responses AND public publish/report: PLAIN publishes the serialized
 * JSON as is (byte for byte unchanged); AES seals the same JSON first. Returns the MQTT message id, or -1.
 */
static int mt_publish_payload(const char *topic, const char *payload, size_t length)
{
#if CONFIG_AIRNGIN_USE_AES
    if (!airngin_clock_trusted()) {
        AG_DIAG_E("AG_AES", "encrypt refused: clock not trusted (no valid envelope timestamp)");
        return -1;
    }
    uint8_t nonce[AIRNGIN_AES_NONCE_LEN];
    esp_fill_random(nonce, sizeof(nonce));
    size_t envelope_len = 0;
    AG_DIAG_I("AG_AES", "encrypt plain_len=%u", (unsigned)length);
    const airngin_aes_result_t r = airngin_aes_seal(mt_aes_key, airngin_internal_serial(), (int64_t)time(NULL), nonce,
                                                    (const uint8_t *)payload, length, mt_aes_envelope,
                                                    sizeof(mt_aes_envelope), &envelope_len);
    airngin_secure_zero(nonce, sizeof(nonce));
    if (r != AIRNGIN_AES_OK) {
        AG_DIAG_E("AG_AES", "encrypt FAILED: %s - message not sent (no plaintext fallback)", airngin_aes_result_name(r));
        return -1;
    }
    AG_DIAG_I("AG_AES", "envelope_len=%u", (unsigned)envelope_len);
    AG_DIAG_I("AG_AES", "publish topic=%s", topic);
    return esp_mqtt_client_publish(mt_client, topic, mt_aes_envelope, (int)envelope_len, 1, 0);
#else
    return esp_mqtt_client_publish(mt_client, topic, payload, (int)length, 1, 0);
#endif
}

#if AG_DIAG
/* What the CURRENT client was created with (so every connect attempt can show it). Passwords are kept only as
 * length + fingerprint, unless the dangerous LOG_SECRETS option is on. */
static char mt_diag_project[24], mt_diag_client[48];
static size_t mt_diag_user_len, mt_diag_pass_len;
static char mt_diag_user_fp[9], mt_diag_pass_fp[9];
#if AG_DIAG_SECRETS
static char mt_diag_user[24], mt_diag_pass[24];
#endif
static unsigned mt_diag_attempt;
#if CONFIG_AIRNGIN_USE_AES
static char mt_diag_aes_fp[9];
#endif
static bool mt_diag_clock_wait_logged;

static const char *mt_diag_connack_name(int code)
{
    switch ((esp_mqtt_connect_return_code_t)code) {
    case MQTT_CONNECTION_ACCEPTED:                 return "ACCEPTED";
    case MQTT_CONNECTION_REFUSE_PROTOCOL:          return "UNACCEPTABLE_PROTOCOL_VERSION";
    case MQTT_CONNECTION_REFUSE_ID_REJECTED:       return "IDENTIFIER_REJECTED";
    case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:return "SERVER_UNAVAILABLE";
    case MQTT_CONNECTION_REFUSE_BAD_USERNAME:      return "BAD_USERNAME_PASSWORD";
    case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:    return "NOT_AUTHORIZED";
    default:                                       return "UNKNOWN";
    }
}

static const char *mt_diag_error_type_name(int type)
{
    switch ((esp_mqtt_error_type_t)type) {
    case MQTT_ERROR_TYPE_NONE:               return "NONE";
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:      return "TCP_TRANSPORT (TLS/socket)";
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED: return "CONNECTION_REFUSED (broker CONNACK)";
    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:   return "SUBSCRIBE_FAILED";
    default:                                 return "UNKNOWN";
    }
}

/* The credentials AirNgin handed to esp-mqtt for the current client. */
static void mt_diag_log_effective(const char *when)
{
    const char *T = "AG_MQTT";
#if AG_DIAG_SECRETS
    ESP_LOGW(T, "========== MQTT EFFECTIVE CREDENTIALS (%s) ==========", when);
    ESP_LOGI(T, "project_code = %s", mt_diag_project);
    ESP_LOGI(T, "broker_host  = %s", mt_broker);
    airngin_diag_field(T, "username", mt_diag_user, mt_diag_user_len, true);
    airngin_diag_field(T, "password", mt_diag_pass, mt_diag_pass_len, true);
    airngin_diag_hex(T, "username", mt_diag_user, mt_diag_user_len);
    airngin_diag_hex(T, "password", mt_diag_pass, mt_diag_pass_len);
    ESP_LOGI(T, "client_id    = %s", mt_diag_client);
    ESP_LOGW(T, "=====================================================");
#else
    ESP_LOGI(T, "credentials handed to esp-mqtt (%s): project=%s host=%s client_id=%s username=<hidden> len=%u fp=%s password=<hidden> len=%u fp=%s",
             when, mt_diag_project, mt_broker, mt_diag_client, (unsigned)mt_diag_user_len, mt_diag_user_fp,
             (unsigned)mt_diag_pass_len, mt_diag_pass_fp);
#endif
}

static void mt_diag_log_error(const esp_mqtt_error_codes_t *e)
{
    const char *T = "AG_MQTT";
    ESP_LOGW(T, "========== MQTT Error ==========");
    ESP_LOGW(T, "error_type            : %d %s", (int)e->error_type, mt_diag_error_type_name((int)e->error_type));
    ESP_LOGW(T, "tls_last_esp_err      : 0x%x (%s)", (unsigned)e->esp_tls_last_esp_err, esp_err_to_name(e->esp_tls_last_esp_err));
    ESP_LOGW(T, "tls_stack_err         : 0x%x", (unsigned)e->esp_tls_stack_err);
    ESP_LOGW(T, "cert_verify_flags     : 0x%x", (unsigned)e->esp_tls_cert_verify_flags);
    ESP_LOGW(T, "socket_errno          : %d", e->esp_transport_sock_errno);
    ESP_LOGW(T, "connection_return_code: %d (%s)", (int)e->connect_return_code, mt_diag_connack_name((int)e->connect_return_code));
    ESP_LOGW(T, "================================");
}
#endif

static void mt_saturating_inc(uint32_t *counter)
{
    portENTER_CRITICAL(&mt_lock);
    if (*counter != UINT32_MAX) ++*counter;
    portEXIT_CRITICAL(&mt_lock);
}

/* Free every queued outbound block; they belong to a session that is over. */
static void mt_tx_drain(void)
{
    if (!mt_tx_queue) return;
    mt_tx *item;
    while (xQueueReceive(mt_tx_queue, &item, 0) == pdTRUE) {
        free(item);
        portENTER_CRITICAL(&mt_lock);
        if (mt_tx_discarded != UINT32_MAX) ++mt_tx_discarded;
        portEXIT_CRITICAL(&mt_lock);
    }
}

static void mt_note_drop(int64_t now)
{
    bool log_it = false;
    uint32_t total;
    portENTER_CRITICAL(&mt_lock);
    total = ++mt_dropped;
    if (mt_drop_logged_us == 0 || now - mt_drop_logged_us >= MT_DROP_LOG_INTERVAL_US) {
        mt_drop_logged_us = now ? now : 1;
        log_it = true;
    }
    portEXIT_CRITICAL(&mt_lock);
    if (log_it) {
        ESP_LOGW("AG_RX", "inbound queue FULL: command dropped (total dropped=%u)",
                 (unsigned)total);
    }
}

/* Which subscription does this MQTT message belong to? Command topic, the ACTIVE time topic, or neither. */
static uint8_t mt_frag_kind = MT_RX_NONE;
static uint8_t mt_classify(const esp_mqtt_event_t *event)
{
    if (!event->topic || event->topic_len <= 0) return MT_RX_NONE;
    if (event->topic_len == (int)mt_in_len && !memcmp(event->topic, mt_in, mt_in_len)) return MT_RX_COMMAND;
    uint8_t kind = MT_RX_NONE;
    portENTER_CRITICAL(&mt_lock);
    if ((mt_time_state == MT_TIME_SUBSCRIBING || mt_time_state == MT_TIME_SUBSCRIBED) && mt_time_active >= 0) {
        const char *topic = mt_time_topic[mt_time_active];
        if ((size_t)event->topic_len == strlen(topic) && !memcmp(event->topic, topic, (size_t)event->topic_len)) {
            kind = mt_time_active == AIRNGIN_TIME_SOURCE_TEHRAN ? MT_RX_TIME_TEHRAN : MT_RX_TIME_UTC;
        }
    }
    portEXIT_CRITICAL(&mt_lock);
#if AG_DIAG
    return kind == MT_RX_NONE ? MT_RX_UNKNOWN : kind;
#else
    return kind;
#endif
}

static void mt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    const int64_t now = esp_timer_get_time();

#if AG_DIAG
    if (id == MQTT_EVENT_BEFORE_CONNECT) AG_DIAG_I("AG_MQTT", "BEFORE_CONNECT (TLS handshake + CONNECT starting)");
#endif
    if (id == MQTT_EVENT_CONNECTED) {
        AG_DIAG_I("AG_MQTT", "CONNECTED (CONNACK accepted)");
        mt_assembling = false;
        portENTER_CRITICAL(&mt_lock);
        mt_connected = true;
        mt_ready = false;
        mt_sub = -1;
        mt_epoch++;
        mt_time_state = MT_TIME_NONE;      /* session-specific subscription state; the application's wish is kept elsewhere */
        mt_time_active = -1;
        mt_time_msg = -1;
        mt_time_retry = 0;
        portEXIT_CRITICAL(&mt_lock);

        const int sub = esp_mqtt_client_subscribe(mt_client, mt_in, 1);
        portENTER_CRITICAL(&mt_lock);
        mt_sub = sub;
        mt_deadline = now + 20000000LL;
        if (sub < 0) mt_failed = true;
        portEXIT_CRITICAL(&mt_lock);
        AG_DIAG_I("AG_MQTT", "SUBSCRIBE request topic=%s qos=1 msg_id=%d%s", mt_in, sub, sub < 0 ? " (REQUEST FAILED)" : "");
    } else if (id == MQTT_EVENT_SUBSCRIBED) {
        AG_DIAG_I("AG_MQTT_RX", "SUBACK msg_id=%d", event->msg_id);
        bool refused = event->error_handle &&
                       event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED;
        if (event->data_len > 0 && event->data && (unsigned char)event->data[0] == 0x80) {
            refused = true;
        }
        AG_DIAG_DECL(bool expected = false)
        bool time_ack = false;
        portENTER_CRITICAL(&mt_lock);
        if (mt_connected && mt_time_state == MT_TIME_SUBSCRIBING && mt_time_msg >= 0 && event->msg_id == mt_time_msg &&
            !(mt_sub >= 0 && event->msg_id == mt_sub)) {
            /* the AirNgin time feed: never touches ready / failed / the core deadline */
            time_ack = true;
            if (refused) {
                mt_time_state = MT_TIME_NONE;
                mt_time_active = -1;
                mt_time_retry = now + MT_TIME_RETRY_US;
            } else {
                mt_time_state = MT_TIME_SUBSCRIBED;
            }
        } else if (mt_connected && mt_sub >= 0 && event->msg_id == mt_sub) {
            AG_DIAG_DO(expected = true);
            if (refused) {
                mt_failed = true;
            } else {
                mt_ready = true;
                mt_notice_ready = true;
                mt_deadline = 0;
            }
        }
        portEXIT_CRITICAL(&mt_lock);
        if (time_ack) {
            if (refused) AG_DIAG_W("AG_TIME_MQTT", "SUBACK msg_id=%d REFUSED; the SDK stays ready, retry in 60 s", event->msg_id);
            else AG_DIAG_I("AG_TIME_MQTT", "SUBACK msg_id=%d", event->msg_id);
        }
#if AG_DIAG
        if (!expected && !time_ack) AG_DIAG_W("AG_MQTT", "SUBACK msg_id=%d ignored (no matching subscription of the current session)", event->msg_id);
        else if (refused) AG_DIAG_E("AG_MQTT", "SUBACK msg_id=%d REFUSED by the broker (subscription not granted)", event->msg_id);
        else {
            AG_DIAG_I("AG_MQTT", "SUBACK received msg_id=%d", event->msg_id);
            AG_DIAG_I("AG_MQTT", "MQTT READY (publish topic=%s)", mt_out);
            mt_diag_attempt = 0;
        }
#endif
    } else if (id == MQTT_EVENT_UNSUBSCRIBED) {
        portENTER_CRITICAL(&mt_lock);
        const bool ours = mt_connected && mt_time_state == MT_TIME_UNSUBSCRIBING && mt_time_msg >= 0 && event->msg_id == mt_time_msg;
        if (ours) {
            mt_time_state = MT_TIME_NONE;
            mt_time_active = -1;
            mt_time_msg = -1;
        }
        portEXIT_CRITICAL(&mt_lock);
        if (ours) AG_DIAG_I("AG_TIME_MQTT", "UNSUBACK msg_id=%d", event->msg_id);
    } else if (id == MQTT_EVENT_PUBLISHED) {
        AG_DIAG_I("AG_MQTT_RX", "PUBACK msg_id=%d", event->msg_id);
    } else if (id == MQTT_EVENT_DISCONNECTED || id == MQTT_EVENT_ERROR) {
        if (id == MQTT_EVENT_DISCONNECTED) AG_DIAG_W("AG_MQTT_RX", "DISCONNECT reason=%d", event->error_handle ? (int)event->error_handle->error_type : 0);
        if (id == MQTT_EVENT_ERROR && event->error_handle && event->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            /* A refused TIME subscription must not take the core session down (the core SUBACK keeps its own handling). */
            portENTER_CRITICAL(&mt_lock);
            const bool time_only = mt_ready && mt_time_state == MT_TIME_SUBSCRIBING;
            if (time_only) {
                mt_time_state = MT_TIME_NONE;
                mt_time_active = -1;
                mt_time_retry = now + MT_TIME_RETRY_US;
            }
            portEXIT_CRITICAL(&mt_lock);
            if (time_only) {
                AG_DIAG_W("AG_TIME_MQTT", "subscription refused by the broker; core session untouched, retry in 60 s");
                return;
            }
        }
#if AG_DIAG
        if (id == MQTT_EVENT_ERROR && event->error_handle) mt_diag_log_error(event->error_handle);
        else AG_DIAG_W("AG_MQTT", "%s%s", id == MQTT_EVENT_ERROR ? "ERROR (no detail available)" : "DISCONNECTED",
                       id == MQTT_EVENT_DISCONNECTED ? " (broker closed the connection or the session ended)" : "");
#endif
        const bool rejected = id == MQTT_EVENT_ERROR && event->error_handle &&
                              event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                              (event->error_handle->connect_return_code == 4 ||
                               event->error_handle->connect_return_code == 5);
        portENTER_CRITICAL(&mt_lock);
        if (mt_ready || mt_connected) mt_notice_disconnected = true;
        mt_connected = false;
        mt_ready = false;
        mt_failed = true;
        mt_epoch++;
        mt_last_reason = (event->error_handle ? (rejected || event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED
                                                      ? event->error_handle->connect_return_code
                                                      : (int)event->error_handle->error_type) : 0);
        if (rejected) {
            mt_rejected = true;
            mt_notice_rejected = true;
        }
        portEXIT_CRITICAL(&mt_lock);
        mt_assembling = false;
#if AG_DIAG
        if (rejected) {
            const int code = (int)event->error_handle->connect_return_code;
            ESP_LOGE("AG_MQTT", "========== Credentials Rejected ==========");
            ESP_LOGE("AG_MQTT", "Project code        : %s", mt_diag_project);
            ESP_LOGE("AG_MQTT", "Device serial       : %s", airngin_internal_serial());
            ESP_LOGE("AG_MQTT", "Client ID           : %s", mt_diag_client);
            ESP_LOGE("AG_MQTT", "Username length     : %u", (unsigned)mt_diag_user_len);
            ESP_LOGE("AG_MQTT", "Password length     : %u", (unsigned)mt_diag_pass_len);
            ESP_LOGE("AG_MQTT", "Password fingerprint: %s", mt_diag_pass_fp);
            ESP_LOGE("AG_MQTT", "Broker response     : %s (code %d)", mt_diag_connack_name(code), code);
            ESP_LOGE("AG_MQTT", "Recovery action     : %s",
                     airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL
                         ? "WAIT FOR airngin_external_set_cloud_credentials() (suspended, no retry)"
                         : "OPEN PROVISIONING (profile flagged for recovery)");
            ESP_LOGE("AG_MQTT", "==========================================");
        }
#endif
    } else if (id == MQTT_EVENT_DATA) {
        portENTER_CRITICAL(&mt_lock);
        const bool ready = mt_ready;
        portEXIT_CRITICAL(&mt_lock);
        if (!ready) {
            mt_assembling = false;
            return;
        }

        if (event->current_data_offset == 0) {
            mt_assembling = false;
            mt_used = 0;
            mt_frag_kind = mt_classify(event);
            const unsigned limit = (mt_frag_kind == MT_RX_TIME_UTC || mt_frag_kind == MT_RX_TIME_TEHRAN)
                                       ? AIRNGIN_TIME_PAYLOAD_MAX : 4096U;
            if (mt_frag_kind == MT_RX_NONE || event->topic_len <= 0 || event->topic_len >= (int)MT_RX_TOPIC_CAPACITY ||
                event->total_data_len <= 0 || (unsigned)event->total_data_len > limit) {
                AG_DIAG_W("AG_RX", "PAYLOAD REJECTED in MQTT callback: missing/oversize topic or payload (topic_len=%d payload_len=%d)",
                          event->topic_len, event->total_data_len);
                return;
            }
#if AG_DIAG
            mt_fragment.topic_len = (uint16_t)event->topic_len;
            memcpy(mt_fragment.topic, event->topic, (size_t)event->topic_len);
            mt_fragment.topic[event->topic_len] = '\0';
#endif
            mt_total = (unsigned)event->total_data_len;
            mt_assembling = true;
        }

        if (!mt_assembling || event->data_len <= 0 || !event->data ||
            event->current_data_offset != (int)mt_used ||
            event->total_data_len != (int)mt_total ||
            (unsigned)event->data_len > mt_total - mt_used) {
            mt_assembling = false;
            return;
        }
        if (event->topic_len > 0 && mt_classify(event) != mt_frag_kind) {
            mt_assembling = false;
            return;
        }

        memcpy(mt_fragment.text + mt_used, event->data, event->data_len);
        mt_used += (unsigned)event->data_len;
        if (mt_used == mt_total) {
            mt_fragment.n = mt_used;
            mt_fragment.kind = mt_frag_kind;
            mt_fragment.text[mt_used] = '\0';
            mt_assembling = false;
            /* Never block the MQTT task: a full queue drops and accounts. */
            if (xQueueSend(mt_queue, &mt_fragment, 0) != pdPASS) {
                if (mt_frag_kind == MT_RX_COMMAND) mt_note_drop(now);
                else AG_DIAG_W("AG_TIME_MQTT", "inbound queue full: time packet dropped");
            } else {
                AG_DIAG_V("AG_RX", "complete message queued (%u bytes)", mt_used);
            }
        }
    }
}

/* Owner task: one AirNgin time packet from the (public, never AES) time topic. */
static void mt_time_dispatch(const mt_message *message)
{
    const airngin_time_source_t source = message->kind == MT_RX_TIME_TEHRAN ? AIRNGIN_TIME_SOURCE_TEHRAN : AIRNGIN_TIME_SOURCE_UTC;
    airngin_time_info_t info;
    AG_DIAG_I("AG_TIME_MQTT", "==================================================");
    AG_DIAG_I("AG_TIME_MQTT", "TIME RECEIVED");
    AG_DIAG_I("AG_TIME_MQTT", "==================================================");
    AG_DIAG_I("AG_TIME_MQTT", "Topic   : %.*s", (int)message->topic_len, message->topic);
    AG_DIAG_I("AG_TIME_MQTT", "Raw (%u bytes):", message->n);
    airngin_diag_print_buffer("AG_TIME_MQTT", (const uint8_t *)message->text, message->n);
    const airngin_time_parse_t r = airngin_time_parse(message->text, message->n, source, &info);
    if (r != AIRNGIN_TIME_PARSE_OK) {
        AG_DIAG_W("AG_TIME_MQTT", "packet rejected: %s", airngin_time_parse_name(r));
        AG_DIAG_I("AG_TIME_MQTT", "==================================================");
        return;
    }
    info.received_at_ms = esp_timer_get_time() / 1000;
    airngin_internal_time_store(&info);
    AG_DIAG_I("AG_TIME_MQTT", "Parsed:");
    AG_DIAG_I("AG_TIME_MQTT", "Source     : %s", source == AIRNGIN_TIME_SOURCE_TEHRAN ? "TEHRAN" : "UTC");
    AG_DIAG_I("AG_TIME_MQTT", "Date       : %04u/%02u/%02u", (unsigned)info.year, (unsigned)info.month, (unsigned)info.day);
    AG_DIAG_I("AG_TIME_MQTT", "Time       : %02u:%02u:%02u",
              (unsigned)info.hour, (unsigned)info.minute, (unsigned)info.second);
    AG_DIAG_I("AG_TIME_MQTT", "DayOfWeek  : %u", (unsigned)info.day_of_week);
    AG_DIAG_I("AG_TIME_MQTT", "DayOfYear  : %u", (unsigned)info.day_of_year);
    AG_DIAG_I("AG_TIME_MQTT", "WeekNumber : %u", (unsigned)info.week_number);
    AG_DIAG_I("AG_TIME_MQTT", "==================================================");
    airngin_event_data_t event = { .type = AIRNGIN_EVENT_TIME_RECEIVED, .error = ESP_OK };
    event.data.time = info;
    airngin_internal_emit(&event);
}

static void mt_dispatch(const mt_message *message)
{
    if (message->kind == MT_RX_UNKNOWN) {
        AG_DIAG_W("AG_RX", "==================================================");
        AG_DIAG_W("AG_RX", "UNKNOWN TOPIC");
        AG_DIAG_W("AG_RX", "==================================================");
        AG_DIAG_W("AG_RX", "Topic       : %.*s", (int)message->topic_len, message->topic);
        AG_DIAG_W("AG_RX", "Payload len : %u", message->n);
        AG_DIAG_W("AG_RX", "Payload:");
        airngin_diag_print_buffer("AG_RX", (const uint8_t *)message->text, message->n);
        AG_DIAG_W("AG_RX", "==================================================");
        return;
    }
    if (message->kind != MT_RX_COMMAND) {
        mt_time_dispatch(message);
        return;
    }
    size_t response_len = 0;
#if AG_DIAG
    ESP_LOGI("AG_RX", "==================================================");
    ESP_LOGI("AG_RX", "MQTT MESSAGE RECEIVED");
    ESP_LOGI("AG_RX", "==================================================");
    ESP_LOGI("AG_RX", "Topic       : %.*s", (int)message->topic_len, message->topic);
    ESP_LOGI("AG_RX", "Topic len   : %u", (unsigned)message->topic_len);
    ESP_LOGI("AG_RX", "Payload len : %u", message->n);
#if !CONFIG_AIRNGIN_USE_AES
    ESP_LOGI("AG_RX", "Payload:");
    airngin_diag_print_buffer("AG_RX", (const uint8_t *)message->text, message->n);
#else
    ESP_LOGI("AG_RX", "Payload      : <AES envelope; plaintext hidden>");
#endif
    ESP_LOGI("AG_RX", "==================================================");
    AG_DIAG_V("AG_RX", "queue depth %u, dropped so far %u", (unsigned)CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH, (unsigned)mt_dropped);
#endif
    const char *logical = message->text;
    size_t logical_len = message->n;
#if CONFIG_AIRNGIN_USE_AES
    {
        /* MQTT payload -> validate envelope -> timestamp/replay -> authenticate+decrypt -> the SAME AirNgin JSON. */
        airngin_aes_info_t info = {0};
        size_t plain_len = 0;
        AG_DIAG_I("AG_AES_RX", "==================================================");
        AG_DIAG_I("AG_AES_RX", "AES MESSAGE RECEIVED");
        AG_DIAG_I("AG_AES_RX", "==================================================");
        AG_DIAG_I("AG_AES_RX", "Topic          : %.*s", (int)message->topic_len, message->topic);
        AG_DIAG_I("AG_AES_RX", "Envelope len   : %u", message->n);
        const airngin_aes_result_t r = airngin_clock_trusted()
            ? airngin_aes_open(mt_aes_key, airngin_internal_serial(), (int64_t)time(NULL), &mt_aes_replay, message->text,
                               message->n, mt_aes_plain, sizeof(mt_aes_plain), &plain_len, &info)
            : AIRNGIN_AES_NO_CLOCK;
        if (r != AIRNGIN_AES_OK) {
            if (mt_aes_rejected != UINT32_MAX) ++mt_aes_rejected;
            ESP_LOGW(TAG, "AES message rejected: %s", airngin_aes_result_name(r));   /* never reveals key material */
            AG_DIAG_W("AG_AES_RX", "Version        : %s", info.version_ok ? AIRNGIN_AES_VERSION : "INVALID/UNAVAILABLE");
            AG_DIAG_W("AG_AES_RX", "Timestamp      : %lld", (long long)info.timestamp);
            AG_DIAG_W("AG_AES_RX", "Nonce len      : %u", (unsigned)info.nonce_len);
            AG_DIAG_W("AG_AES_RX", "Ciphertext len : %u", (unsigned)info.ciphertext_b64_len);
            AG_DIAG_W("AG_AES_RX", "Tag len        : %u", (unsigned)info.tag_len);
            AG_DIAG_W("AG_AES_RX", "Authentication : FAIL (%s)", airngin_aes_result_name(r));
            AG_DIAG_W("AG_AES_RX", "RX rejected: %s (total rejected=%u); command handler NOT called, no response",
                      airngin_aes_result_name(r), (unsigned)mt_aes_rejected);
            AG_DIAG_I("AG_AES_RX", "==================================================");
            airngin_secure_zero(mt_aes_plain, sizeof(mt_aes_plain));
            return;
        }
        AG_DIAG_I("AG_AES_RX", "Version        : %s", AIRNGIN_AES_VERSION);
        AG_DIAG_I("AG_AES_RX", "Timestamp      : %lld", (long long)info.timestamp);
        AG_DIAG_I("AG_AES_RX", "Nonce len      : %u", (unsigned)info.nonce_len);
        AG_DIAG_I("AG_AES_RX", "Ciphertext len : %u", (unsigned)info.ciphertext_b64_len);
        AG_DIAG_I("AG_AES_RX", "Tag len        : %u", (unsigned)info.tag_len);
        AG_DIAG_I("AG_AES_RX", "Authentication : PASS");
#if AG_DIAG_SECRETS
        AG_DIAG_I("AG_AES_RX", "DECRYPTED PAYLOAD (%u bytes):", (unsigned)plain_len);
        airngin_diag_print_buffer("AG_AES_RX", mt_aes_plain, plain_len);
#endif
        AG_DIAG_I("AG_AES_RX", "==================================================");
        logical = (const char *)mt_aes_plain;
        logical_len = plain_len;
    }
#endif
    const esp_err_t err = airngin_protocol_process(logical, logical_len,
                                                    mt_response, sizeof(mt_response),
                                                    &response_len);
    if (err != ESP_OK) {
        AG_DIAG_W("AG_RX", "PAYLOAD REJECTED");
        AG_DIAG_W("AG_RX", "Reason : invalid UTF-8 / JSON / schema / command result (see AG_PROTO)");
        AG_DIAG_W("AG_RX", "Topic  : %.*s", (int)message->topic_len, message->topic);
        AG_DIAG_W("AG_RX", "Length : %u", (unsigned)logical_len);
    }
#if CONFIG_AIRNGIN_USE_AES
    airngin_secure_zero(mt_aes_plain, sizeof(mt_aes_plain));   /* the decrypted command is no longer needed */
#endif
    if (err == ESP_OK && response_len > 0U) {
        AG_DIAG_I("AG_TX", "command response built len=%u", (unsigned)response_len);
        AG_DIAG_I("AG_TX", "topic=%s", mt_out);
        const int msg_id = mt_publish_payload(mt_out, mt_response, response_len);
        if (msg_id < 0) {
            AG_DIAG_E("AG_TX", "command response publish FAILED (client busy or disconnected)");
            ESP_LOGW(TAG, "Reply publish failed (client busy or disconnected); reply dropped");
        } else {
            AG_DIAG_I("AG_TX", "publish result msg_id=%d", msg_id);
#if !CONFIG_AIRNGIN_USE_AES || AG_DIAG_SECRETS
            AG_DIAG_V("AG_TX", "response text: %.*s", (int)response_len, mt_response);
#endif
        }
#if CONFIG_AIRNGIN_USE_AES
        airngin_secure_zero(mt_response, sizeof(mt_response));
#endif
    } else if (err != ESP_OK && err != ESP_ERR_INVALID_ARG && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Command ignored: " AG_ERRF, AG_ERRV(err));
    } else if (err == ESP_OK) {
        AG_DIAG_I("AG_TX", "no response to publish (handler produced none)");
    } else {
        AG_DIAG_W("AG_RX", "command not executed: " AG_ERRF, AG_ERRV(err));
    }
#if CONFIG_AIRNGIN_STACK_DIAGNOSTICS
    /* Debug aid only (off by default): one line per processed command. */
    ESP_LOGI(TAG, "owner stack high-water mark: %u bytes never used",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
}

static void mt_emit_pending_events(void)
{
    bool ready_notice, disconnected_notice, rejected_notice;
    int reason;
    uint32_t discarded;
    /* The session is over: queued public publishes are stale. Drain them first so the event
     * reports every message that was discarded with this session. */
    portENTER_CRITICAL(&mt_lock);
    const bool session_ended = mt_notice_disconnected;
    portEXIT_CRITICAL(&mt_lock);
    if (session_ended) mt_tx_drain();
    portENTER_CRITICAL(&mt_lock);
    ready_notice = mt_notice_ready;
    disconnected_notice = mt_notice_disconnected;
    rejected_notice = mt_notice_rejected;
    reason = mt_last_reason;
    discarded = mt_tx_discarded;
    mt_notice_ready = false;
    mt_notice_disconnected = false;
    mt_notice_rejected = false;
    if (disconnected_notice) mt_tx_discarded = 0;
    portEXIT_CRITICAL(&mt_lock);

    airngin_event_data_t event = { .type = AIRNGIN_EVENT_MQTT_DISCONNECTED, .error = ESP_OK };
    if (disconnected_notice) {
        event.data.mqtt.reason = reason;
        event.data.mqtt.tx_discarded = discarded;
        airngin_internal_emit(&event);
    }
    if (ready_notice) {
        memset(&event, 0, sizeof(event));
        event.type = AIRNGIN_EVENT_MQTT_READY;
        airngin_internal_emit(&event);
    }
    if (rejected_notice) {
        memset(&event, 0, sizeof(event));
        event.type = AIRNGIN_EVENT_CREDENTIALS_REJECTED;
        event.data.mqtt.reason = reason;
        airngin_internal_emit(&event);
    }
}

/* Owner task only: transmit queued public publishes while the session is ready. */
static void mt_tx_service(void)
{
    mt_tx *item;
    while (mt_tx_queue && xQueueReceive(mt_tx_queue, &item, 0) == pdTRUE) {
        portENTER_CRITICAL(&mt_lock);
        const bool current = mt_ready && item->epoch == mt_epoch;
        portEXIT_CRITICAL(&mt_lock);
        if (!current) {
            portENTER_CRITICAL(&mt_lock);
            if (mt_tx_discarded != UINT32_MAX) ++mt_tx_discarded;
            portEXIT_CRITICAL(&mt_lock);
            free(item);
            continue;
        }
        size_t length = 0;
        const char *key = item->data;
        const char *value = item->data + item->key_len + 1U;
        if (item->kind != MT_TX_REPORT) {
            /* Notification: the owner task builds payload and topic. ESP_OK upstream means "queued", never "delivered". */
            const bool to_user = item->kind == MT_TX_NOTIFY_USER;
            const char *topic = to_user ? mt_notify_user : mt_notify_center;
            const esp_err_t built = airngin_notify_build(to_user, key, item->channel, mt_response, sizeof(mt_response), &length);
            AG_DIAG_I("AG_NOTIFY", "topic=%s", topic);
            AG_DIAG_V("AG_NOTIFY", "code=\"%.64s\"", key);
#if CONFIG_AIRNGIN_SERIAL_TEST_CONSOLE
            if (built == ESP_OK) {
                ESP_LOGI("AG_NOTIFY_TX", "Target  : %s", to_user ? "USER" : "CENTER");
                if (to_user) {
                    ESP_LOGI("AG_NOTIFY_TX", "Channel : %s", item->channel == AIRNGIN_NOTIFICATION_SMS ? "SMS" : "PUSH");
                    ESP_LOGI("AG_NOTIFY_TX", "Type    : %d", item->channel);
                }
                ESP_LOGI("AG_NOTIFY_TX", "Topic   : %s", topic);
                ESP_LOGI("AG_NOTIFY_TX", "Payload : %.*s", (int)length, mt_response);
                ESP_LOGI("AG_NOTIFY_TX", "QoS     : 1");
            }
#endif
            const int id = built == ESP_OK ? mt_publish_payload(topic, mt_response, length) : -1;
            if (built != ESP_OK || id < 0) {
                mt_saturating_inc(&mt_tx_failed);
                AG_DIAG_E("AG_NOTIFY", "publish FAILED (%s)", built != ESP_OK ? "serialization" : "esp_mqtt_client_publish");
                ESP_LOGW(TAG, "Notification publish failed; message dropped");
            } else {
                AG_DIAG_I("AG_NOTIFY", "MQTT publish msg_id=%d len=%u", id, (unsigned)length);
            }
#if CONFIG_AIRNGIN_USE_AES
            airngin_secure_zero(mt_response, sizeof(mt_response));
#endif
            free(item);
            continue;
        }
#if !CONFIG_AIRNGIN_USE_AES || AG_DIAG_SECRETS
        AG_DIAG_V("AG_TX", "sending queued publish key=\"%.64s\" topic=%s", key, mt_out);
#else
        AG_DIAG_V("AG_TX", "sending queued publish (AES; key/value hidden) topic=%s", mt_out);
#endif
        const esp_err_t build_err = airngin_protocol_build_report(key, value, mt_response, sizeof(mt_response), &length);
        const int pub_id = build_err == ESP_OK ? mt_publish_payload(mt_out, mt_response, length) : -1;
        if (build_err != ESP_OK || pub_id < 0) {
            mt_saturating_inc(&mt_tx_failed);
            AG_DIAG_E("AG_TX", "queued publish FAILED (%s)", build_err != ESP_OK ? "serialization" : "esp_mqtt_client_publish");
            ESP_LOGW(TAG, "Public publish failed; message dropped");
        } else {
            AG_DIAG_I("AG_TX", "queued publish sent: msg_id=%d len=%u", pub_id, (unsigned)length);
        }
#if CONFIG_AIRNGIN_USE_AES
        airngin_secure_zero(mt_response, sizeof(mt_response));
#endif
        free(item);
    }
}

static esp_err_t mt_enqueue(mt_tx_kind kind, uint8_t channel, const char *key, size_t key_len,
                            const char *value, size_t value_len)
{
    portENTER_CRITICAL(&mt_lock);
    if (!mt_tx_accepting || !mt_ready || !mt_tx_queue) {
        portEXIT_CRITICAL(&mt_lock);
        return ESP_ERR_INVALID_STATE;
    }
    QueueHandle_t queue = mt_tx_queue;
    const uint32_t epoch = mt_epoch;
    mt_tx_inflight++;
    portEXIT_CRITICAL(&mt_lock);

    esp_err_t err = ESP_OK;
    mt_tx *item = malloc(sizeof(*item) + key_len + value_len + 2U);
    if (!item) {
        err = ESP_ERR_NO_MEM;
    } else {
        item->epoch = epoch;
        item->kind = (uint8_t)kind;
        item->channel = channel;
        item->key_len = (uint16_t)key_len;
        item->value_len = (uint16_t)value_len;
        memcpy(item->data, key, key_len);
        item->data[key_len] = '\0';
        memcpy(item->data + key_len + 1U, value, value_len);
        item->data[key_len + 1U + value_len] = '\0';
        if (xQueueSend(queue, &item, 0) != pdPASS) {
            free(item);   /* not queued: still ours */
            err = ESP_ERR_NO_MEM;
        }
    }

    portENTER_CRITICAL(&mt_lock);
    mt_tx_inflight--;
    if (err != ESP_OK && mt_tx_rejected != UINT32_MAX) ++mt_tx_rejected;
    AG_DIAG_DECL(const uint32_t rejected_total = mt_tx_rejected)
    portEXIT_CRITICAL(&mt_lock);
    if (err != ESP_OK) AG_DIAG_W("AG_TX", "outgoing queue FULL or no memory: rejected=%u (" AG_ERRF ")", (unsigned)rejected_total, AG_ERRV(err));
    return err;
}

esp_err_t airngin_transport_enqueue_publish(const char *key, size_t key_len,
                                            const char *value, size_t value_len)
{
    if (!key || !value || key_len == 0U || key_len > AIRNGIN_COMMAND_KEY_MAX ||
        value_len > AIRNGIN_COMMAND_VALUE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return mt_enqueue(MT_TX_REPORT, 0, key, key_len, value, value_len);
}

esp_err_t airngin_transport_enqueue_notification(bool to_user, int channel, const char *code, size_t code_len)
{
    if (!code || code_len == 0U || code_len > AIRNGIN_NOTIFICATION_CODE_MAX || (to_user && !airngin_notify_channel_valid(channel))) {
        return ESP_ERR_INVALID_ARG;
    }
    return mt_enqueue(to_user ? MT_TX_NOTIFY_USER : MT_TX_NOTIFY_CENTER, to_user ? (uint8_t)channel : 0U, code, code_len, "", 0U);
}

bool airngin_transport_connected(void)
{
    portENTER_CRITICAL(&mt_lock);
    const bool connected = mt_connected;
    portEXIT_CRITICAL(&mt_lock);
    return connected;
}

bool airngin_transport_ready(void)
{
    portENTER_CRITICAL(&mt_lock);
    const bool ready = mt_ready;
    portEXIT_CRITICAL(&mt_lock);
    return ready;
}

void airngin_transport_counters(uint32_t *rx_dropped, uint32_t *tx_rejected, uint32_t *tx_failed)
{
    portENTER_CRITICAL(&mt_lock);
    if (rx_dropped) *rx_dropped = mt_dropped;
    if (tx_rejected) *tx_rejected = mt_tx_rejected;
    if (tx_failed) *tx_failed = mt_tx_failed;
    portEXIT_CRITICAL(&mt_lock);
}

uint32_t airngin_transport_dropped_count(void)
{
    portENTER_CRITICAL(&mt_lock);
    const uint32_t total = mt_dropped;
    portEXIT_CRITICAL(&mt_lock);
    return total;
}

static void mt_clear_effective_profile(void)
{
    portENTER_CRITICAL(&mt_lock);
    airngin_secure_zero(&mt_effective_profile, sizeof(mt_effective_profile));
    mt_effective_profile_valid = false;
    portEXIT_CRITICAL(&mt_lock);
}

esp_err_t airngin_transport_get_effective_cloud_profile(airngin_profile *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&mt_lock);
    const bool valid = mt_effective_profile_valid;
    if (valid) *out = mt_effective_profile;
    portEXIT_CRITICAL(&mt_lock);
    if (!valid) {
        memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/*
 * Copy one validated profile into the single effective identity, then build topics and create ESP-MQTT from that copy.
 * Used by init and by credential replacement, so a replaced project code / broker / client id can never
 * leave a cached topic or client behind. Leaves mt_client NULL on failure.
 */
static esp_err_t mt_open_client(const airngin_profile *profile)
{
    mt_clear_effective_profile();
    portENTER_CRITICAL(&mt_lock);
    mt_effective_profile = *profile;
    portEXIT_CRITICAL(&mt_lock);
    const airngin_profile *effective = &mt_effective_profile;

    char in[sizeof(mt_in)], out[sizeof(mt_out)];
    const int in_chars = airngin_topic_build(AIRNGIN_TOPIC_SERVER_TO_DEVICE, MT_USE_AES, effective->projectCode,
                                             airngin_internal_serial(), in, sizeof(in));
    const int out_chars = airngin_topic_build(AIRNGIN_TOPIC_DEVICE_TO_SERVER, MT_USE_AES, effective->projectCode,
                                              airngin_internal_serial(), out, sizeof(out));
    char notify_center[sizeof(mt_notify_center)], notify_user[sizeof(mt_notify_user)];
    char time_topic[2][sizeof(mt_time_topic[0])];
    if (in_chars < 0 || out_chars < 0 ||
        airngin_topic_build(AIRNGIN_TOPIC_NOTIFY_CENTER, MT_USE_AES, effective->projectCode, NULL, notify_center, sizeof(notify_center)) < 0 ||
        airngin_topic_build(AIRNGIN_TOPIC_NOTIFY_USER, MT_USE_AES, effective->projectCode, NULL, notify_user, sizeof(notify_user)) < 0 ||
        airngin_topic_build(AIRNGIN_TOPIC_TIME_UTC, MT_USE_AES, effective->projectCode, NULL, time_topic[0], sizeof(time_topic[0])) < 0 ||
        airngin_topic_build(AIRNGIN_TOPIC_TIME_TEHRAN, MT_USE_AES, effective->projectCode, NULL, time_topic[1], sizeof(time_topic[1])) < 0) {
        mt_clear_effective_profile();
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(mt_notify_center, notify_center, sizeof(mt_notify_center));
    memcpy(mt_notify_user, notify_user, sizeof(mt_notify_user));
    memcpy(mt_time_topic, time_topic, sizeof(mt_time_topic));
#if CONFIG_AIRNGIN_USE_AES
    /* AES build: the key must be valid BEFORE any client exists. A new key means a new replay window. */
    if (!airngin_aes_key_decode(effective->aesKey, mt_aes_key)) {
        AG_DIAG_E("AG_AES", "AES key missing or invalid (needs Base64 of exactly 32 bytes): client not created");
        mt_aes_wipe();
        mt_clear_effective_profile();
        return ESP_ERR_INVALID_ARG;
    }
    airngin_aes_replay_reset(&mt_aes_replay);
#endif
    memcpy(mt_in, in, (size_t)in_chars + 1U);
    memcpy(mt_out, out, (size_t)out_chars + 1U);
    mt_in_len = (size_t)in_chars;
    airngin_secure_zero(mt_broker, sizeof(mt_broker));          /* no remnant of a previous, longer host name */
    memcpy(mt_broker, effective->mqttBroker, strlen(effective->mqttBroker) + 1U);

    const esp_mqtt_client_config_t config = {
        .broker.address.hostname = mt_broker,
        .broker.address.port = MT_BROKER_PORT,
        .broker.address.transport = MT_BROKER_TRANSPORT,
        .broker.verification.certificate = (const char *)airngin_root_ca_start,
        .credentials.username = effective->mqttUser,
        .credentials.authentication.password = effective->mqttPass,
        .credentials.client_id = effective->clientId,
        .network.disable_auto_reconnect = true,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
    };

#if AG_DIAG
    memset(mt_diag_project, 0, sizeof(mt_diag_project));
    memset(mt_diag_client, 0, sizeof(mt_diag_client));
    memcpy(mt_diag_project, effective->projectCode, sizeof(effective->projectCode));
    memcpy(mt_diag_client, effective->clientId, sizeof(effective->clientId));
    mt_diag_user_len = strlen(effective->mqttUser);
    mt_diag_pass_len = strlen(effective->mqttPass);
    airngin_diag_fingerprint(effective->mqttUser, mt_diag_user_len, mt_diag_user_fp);
    airngin_diag_fingerprint(effective->mqttPass, mt_diag_pass_len, mt_diag_pass_fp);
#if CONFIG_AIRNGIN_USE_AES
    airngin_diag_fingerprint(effective->aesKey, strnlen(effective->aesKey, sizeof(effective->aesKey)), mt_diag_aes_fp);
#endif
#if AG_DIAG_SECRETS
    memset(mt_diag_user, 0, sizeof(mt_diag_user));
    memset(mt_diag_pass, 0, sizeof(mt_diag_pass));
    memcpy(mt_diag_user, effective->mqttUser, sizeof(effective->mqttUser));
    memcpy(mt_diag_pass, effective->mqttPass, sizeof(effective->mqttPass));
#endif
    mt_diag_attempt = 0;
    mt_diag_clock_wait_logged = false;
    mt_diag_log_effective("client created");
#endif
    mt_client = esp_mqtt_client_init(&config);
    if (!mt_client) {
        AG_DIAG_E("AG_MQTT", "esp_mqtt_client_init failed (out of memory)");
#if CONFIG_AIRNGIN_USE_AES
        mt_aes_wipe();
#endif
        mt_clear_effective_profile();
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t err = esp_mqtt_client_register_event(mt_client, ESP_EVENT_ANY_ID, mt_event, NULL);
    if (err != ESP_OK) {
        AG_DIAG_E("AG_MQTT", "event registration failed " AG_ERRF, AG_ERRV(err));
        (void)esp_mqtt_client_destroy(mt_client);
        mt_client = NULL;
#if CONFIG_AIRNGIN_USE_AES
        mt_aes_wipe();
#endif
        mt_clear_effective_profile();
        return err;
    }
    portENTER_CRITICAL(&mt_lock);
    mt_effective_profile_valid = true;
    portEXIT_CRITICAL(&mt_lock);
    AG_DIAG_I("AG_MQTT", "client created: host=%s port=%u TLS=1 (certificate + hostname verification)", mt_broker, (unsigned)MT_BROKER_PORT);
    AG_DIAG_I("AG_MQTT", "publish topic   = %s", mt_out);
    AG_DIAG_I("AG_MQTT", "subscribe topic = %s", mt_in);
#if CONFIG_AIRNGIN_USE_AES
    AG_DIAG_I("AG_AES", "application layer AES-256-GCM enabled; key source=provisioning/credentials, length=%u bytes, fp=%s (key never printed)",
              (unsigned)AIRNGIN_AES_KEY_LEN, mt_diag_aes_fp);
#else
    AG_DIAG_I("AG_PROTO", "application encryption disabled (PLAIN); MQTT is still TLS-protected");
#endif
    return ESP_OK;
}

esp_err_t airngin_transport_init(const airngin_profile *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    if (mt_client) return ESP_ERR_INVALID_STATE;

    const size_t ca_size = (size_t)(airngin_root_ca_end - airngin_root_ca_start);
    if (ca_size < 2U || airngin_root_ca_start[ca_size - 1U] != 0) return ESP_ERR_INVALID_SIZE;

    if (mt_queue) {
        vQueueDelete(mt_queue);
        mt_queue = NULL;
    }
    mt_queue = xQueueCreate(CONFIG_AIRNGIN_COMMAND_QUEUE_DEPTH, sizeof(mt_message));
    if (!mt_queue) return ESP_ERR_NO_MEM;

    if (mt_tx_queue) {
        mt_tx_drain();
        vQueueDelete(mt_tx_queue);
        mt_tx_queue = NULL;
    }
    mt_tx_queue = xQueueCreate(CONFIG_AIRNGIN_TX_QUEUE_DEPTH, sizeof(mt_tx *));
    if (!mt_tx_queue) {
        (void)airngin_transport_destroy();
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&mt_lock);
    mt_dropped = 0;
    mt_drop_logged_us = 0;
    mt_stop_failures = 0;
    mt_tx_rejected = mt_tx_failed = mt_tx_discarded = 0;
    mt_last_reason = 0;
    mt_tx_inflight = 0;
    mt_tx_accepting = true;
    mt_start_gate = false;
    portEXIT_CRITICAL(&mt_lock);

    const esp_err_t open_err = mt_open_client(profile);
    if (open_err != ESP_OK) {
        (void)airngin_transport_destroy();
        return open_err;
    }
    return ESP_OK;
}

esp_err_t airngin_transport_stop(void)
{
    portENTER_CRITICAL(&mt_lock);
    const bool running = mt_running;
    const bool was_online = mt_ready || mt_connected;
    mt_ready = false;
    mt_connected = false;
    if (was_online) mt_notice_disconnected = true;
    portEXIT_CRITICAL(&mt_lock);

    if (running && mt_client) {
        const esp_err_t err = esp_mqtt_client_stop(mt_client);
        if (err != ESP_OK) {
            /* Not stopped: keep started/running so a later call retries. */
            portENTER_CRITICAL(&mt_lock);
            mt_failed = true;
            const unsigned failures = ++mt_stop_failures;
            portEXIT_CRITICAL(&mt_lock);
            if (failures <= MT_STOP_RETRY_MAX) {
                ESP_LOGW(TAG, "esp_mqtt_client_stop failed: %s", esp_err_to_name(err));
            }
            return err;
        }
    }

    portENTER_CRITICAL(&mt_lock);
    mt_started = false;
    mt_running = false;
    mt_ready = false;
    mt_connected = false;
    mt_failed = false;
    mt_sub = -1;
    mt_deadline = 0;
    mt_stop_failures = 0;
    mt_time_state = MT_TIME_NONE;      /* session over: the wish survives in airngin.c, the broker side does not */
    mt_time_active = -1;
    mt_time_msg = -1;
    mt_time_retry = 0;
    mt_epoch++;
    portEXIT_CRITICAL(&mt_lock);
    if (running) AG_DIAG_I("AG_MQTT", "client stopped (session closed, queued publishes discarded)");
    mt_assembling = false;
    if (mt_queue) xQueueReset(mt_queue);
    mt_tx_drain();   /* queued public publishes belong to the finished session */
    return ESP_OK;
}


void airngin_transport_set_start_gate(bool blocked)
{
    portENTER_CRITICAL(&mt_lock);
    mt_start_gate = blocked;
    portEXIT_CRITICAL(&mt_lock);
}

void airngin_transport_hold_until(int64_t not_before_us)
{
    portENTER_CRITICAL(&mt_lock);
    mt_next = not_before_us;
    portEXIT_CRITICAL(&mt_lock);
}

esp_err_t airngin_transport_drop_session(void)
{
    const esp_err_t err = airngin_transport_stop();
    /* A READY that was never delivered belongs to the session that just ended. */
    portENTER_CRITICAL(&mt_lock);
    mt_notice_ready = false;
    portEXIT_CRITICAL(&mt_lock);
    return err;
}

esp_err_t airngin_transport_replace_credentials(const airngin_profile *profile, int64_t not_before_us)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    if (!mt_tx_queue || !mt_queue) return ESP_ERR_INVALID_STATE;   /* transport was never initialized */

    /* 1. End the old session (drains queued TX under the usual discard policy). Nothing else is
     *    touched if it cannot be stopped; the caller retries. */
    esp_err_t err = airngin_transport_drop_session();
    if (err != ESP_OK) return err;

    /* 2. The old client carries the old identity/broker: never reuse it. */
    if (mt_client) {
        err = esp_mqtt_client_destroy(mt_client);
        if (err != ESP_OK) return err;
        mt_client = NULL;
        mt_clear_effective_profile();
    }

    /* 3. New topics, host and client. Queues and diagnostic counters are deliberately kept. */
    err = mt_open_client(profile);
    portENTER_CRITICAL(&mt_lock);
    mt_rejected = false;        /* a different credential generation may try again */
    mt_failed = false;
    mt_started = false;
    mt_running = false;
    mt_deadline = 0;
    mt_stop_failures = 0;
    mt_next = not_before_us;
    portEXIT_CRITICAL(&mt_lock);
    return err;
}

void airngin_transport_wifi_recovered(int64_t now)
{
    (void)airngin_transport_stop();
    portENTER_CRITICAL(&mt_lock);
    mt_next = now + 60000000LL;
    portEXIT_CRITICAL(&mt_lock);
}

/*
 * Owner task, only while the core session is READY: bring the broker-side time subscription in line with the
 * application's wish. Never blocks; SUBACK/UNSUBACK arrive through the MQTT callback and only update mt_time_*.
 * Switching sources is sequential: UNSUBSCRIBE the old topic, wait for its UNSUBACK (or 10 s), then SUBSCRIBE the new one.
 */
static void mt_time_service(int64_t now)
{
    airngin_time_source_t want_source = AIRNGIN_TIME_SOURCE_UTC;
    const bool want = airngin_internal_time_wanted(&want_source);

    portENTER_CRITICAL(&mt_lock);
    const mt_time_state_t state = mt_time_state;
    const int active = mt_time_active;
    const int64_t deadline = mt_time_deadline;
    const int64_t retry = mt_time_retry;
    portEXIT_CRITICAL(&mt_lock);

    if (state == MT_TIME_NONE) {
        if (!want || now < retry) return;
        const int id = esp_mqtt_client_subscribe(mt_client, mt_time_topic[want_source], 0);
        AG_DIAG_I("AG_TIME_MQTT", "subscribing topic=%s qos=0 msg_id=%d%s", mt_time_topic[want_source], id, id < 0 ? " (REQUEST FAILED)" : "");
        portENTER_CRITICAL(&mt_lock);
        if (id >= 0) {
            mt_time_state = MT_TIME_SUBSCRIBING;
            mt_time_active = (int)want_source;
            mt_time_msg = id;
            mt_time_deadline = now + MT_TIME_ACK_TIMEOUT_US;
        } else {
            mt_time_retry = now + MT_TIME_RETRY_US;
        }
        portEXIT_CRITICAL(&mt_lock);
    } else if (state == MT_TIME_SUBSCRIBING) {
        if (now >= deadline) {
            portENTER_CRITICAL(&mt_lock);
            mt_time_state = MT_TIME_NONE;
            mt_time_active = -1;
            mt_time_msg = -1;
            mt_time_retry = now + MT_TIME_RETRY_US;
            portEXIT_CRITICAL(&mt_lock);
            AG_DIAG_W("AG_TIME_MQTT", "no SUBACK within 10 s; core session untouched, retry in 60 s");
        }
    } else if (state == MT_TIME_SUBSCRIBED) {
        if (active >= 0 && (!want || (int)want_source != active)) {
            const int id = esp_mqtt_client_unsubscribe(mt_client, mt_time_topic[active]);
            AG_DIAG_I("AG_TIME_MQTT", "unsubscribing topic=%s msg_id=%d%s", mt_time_topic[active], id, id < 0 ? " (REQUEST FAILED)" : "");
            portENTER_CRITICAL(&mt_lock);
            if (id >= 0) {
                mt_time_state = MT_TIME_UNSUBSCRIBING;
                mt_time_msg = id;
                mt_time_deadline = now + MT_TIME_ACK_TIMEOUT_US;
            } else {
                mt_time_state = MT_TIME_NONE;      /* inbound time messages are filtered by the active topic anyway */
                mt_time_active = -1;
                mt_time_msg = -1;
            }
            portEXIT_CRITICAL(&mt_lock);
        }
    } else if (now >= deadline) {   /* UNSUBSCRIBING without an UNSUBACK: carry on */
        portENTER_CRITICAL(&mt_lock);
        mt_time_state = MT_TIME_NONE;
        mt_time_active = -1;
        mt_time_msg = -1;
        portEXIT_CRITICAL(&mt_lock);
        AG_DIAG_W("AG_TIME_MQTT", "no UNSUBACK within 10 s; continuing");
    }
}

int airngin_transport_tick(bool ip, int64_t now)
{
    if (!mt_client) return 0;

    airngin_database_poll();      /* a finished Cloud Database request is delivered here, on the owner task */
    mt_emit_pending_events();

    portENTER_CRITICAL(&mt_lock);
    const bool rejected = mt_rejected;
    const bool started = mt_started;
    const bool ready = mt_ready;
    const bool failed = mt_failed;
    const int64_t deadline = mt_deadline;
    const int64_t next = mt_next;
    const bool gave_up = mt_stop_failures > MT_STOP_RETRY_MAX;
    const bool start_gated = mt_start_gate;
    portEXIT_CRITICAL(&mt_lock);

    if (rejected) {
        (void)airngin_transport_stop();
        mt_emit_pending_events();
        return 2;
    }

    /* A client that cannot be stopped is only resolved by transport_destroy(). */
    if (gave_up) return 0;

    const bool trusted = airngin_clock_trusted();
#if AG_DIAG
    if (!started && ip && !trusted && now >= next) {
        if (!mt_diag_clock_wait_logged) {
            AG_DIAG_W("AG_TIME", "clock not trusted; MQTT postponed until the system time is trusted");
            mt_diag_clock_wait_logged = true;
        }
    } else if (trusted) {
        mt_diag_clock_wait_logged = false;
    }
#endif
    if (started && (!ip || !trusted || failed || (deadline && now >= deadline))) {
#if AG_DIAG
        {
            portENTER_CRITICAL(&mt_lock);
            const bool was_connected = mt_connected;
            portEXIT_CRITICAL(&mt_lock);
            AG_DIAG_W("AG_MQTT", "ending session: %s", !ip ? "network lost" : !trusted ? "clock no longer trusted" : failed ? "session failed or disconnected (see the events above)"
                      : was_connected ? "SUBACK timeout after 20000 ms" : "connection timeout after 20000 ms");
        }
#endif
        (void)airngin_transport_stop();
        portENTER_CRITICAL(&mt_lock);
        mt_next = now + 60000000LL;
        portEXIT_CRITICAL(&mt_lock);
        mt_emit_pending_events();
        return 0;
    }

#if AG_DIAG
    {
        static bool gate_logged;
        if (!started && ip && trusted && now >= next && start_gated) {
            if (!gate_logged) {
                AG_DIAG_I("AG_MQTT", "MQTT start suppressed: managed provisioning is active/requested/transitioning");
                gate_logged = true;
            }
        } else if (!start_gated) {
            gate_logged = false;
        }
    }
#endif
    if (!started && ip && trusted && now >= next && !start_gated) {
        portENTER_CRITICAL(&mt_lock);
        mt_started = true;
        mt_failed = false;
        mt_deadline = now + 20000000LL;
        portEXIT_CRITICAL(&mt_lock);

        ESP_LOGI(TAG, "Connecting MQTT broker host=%s port=%d transport=TLS",
                 mt_broker, MT_BROKER_PORT);
#if AG_DIAG
        ++mt_diag_attempt;
        ESP_LOGI("AG_MQTT", "========== MQTT Connect Attempt ==========");
        ESP_LOGI("AG_MQTT", "Attempt             : %u", mt_diag_attempt);
        ESP_LOGI("AG_MQTT", "Host                : %s", mt_broker);
        ESP_LOGI("AG_MQTT", "Port                : %u", (unsigned)MT_BROKER_PORT);
        ESP_LOGI("AG_MQTT", "Transport           : TLS");
        ESP_LOGI("AG_MQTT", "Protocol            : MQTT 3.1.1");
        ESP_LOGI("AG_MQTT", "Credential source   : %s", airngin_internal_network_mode() == AIRNGIN_NETWORK_EXTERNAL
                                                          ? "External config" : "Managed NVS profile");
        ESP_LOGI("AG_MQTT", "Project code        : %s", mt_diag_project);
        ESP_LOGI("AG_MQTT", "Client ID           : %s", mt_diag_client);
        ESP_LOGI("AG_MQTT", "Username length     : %u", (unsigned)mt_diag_user_len);
        ESP_LOGI("AG_MQTT", "Username fingerprint: %s", mt_diag_user_fp);
        ESP_LOGI("AG_MQTT", "Password length     : %u", (unsigned)mt_diag_pass_len);
        ESP_LOGI("AG_MQTT", "Password fingerprint: %s", mt_diag_pass_fp);
        ESP_LOGI("AG_MQTT", "Clock trusted       : %s", airngin_diag_yesno(trusted));
        ESP_LOGI("AG_MQTT", "Network connected   : %s", airngin_diag_yesno(ip));
        ESP_LOGI("AG_MQTT", "==========================================");
        mt_diag_log_effective("about to connect");
#endif
        if (esp_mqtt_client_start(mt_client) == ESP_OK) {
            portENTER_CRITICAL(&mt_lock);
            mt_running = true;
            portEXIT_CRITICAL(&mt_lock);
        } else {
            /* Never started: there is nothing for esp_mqtt_client_stop() to stop. */
            portENTER_CRITICAL(&mt_lock);
            mt_started = false;
            mt_failed = false;
            mt_deadline = 0;
            mt_next = now + 60000000LL;
            portEXIT_CRITICAL(&mt_lock);
            AG_DIAG_E("AG_MQTT", "esp_mqtt_client_start FAILED; next attempt in 60 s");
        }
    }

    if (ready) {
        while (xQueueReceive(mt_queue, &mt_rx, 0) == pdTRUE) {
            portENTER_CRITICAL(&mt_lock);
            const bool current = mt_ready;
            portEXIT_CRITICAL(&mt_lock);
            if (current) mt_dispatch(&mt_rx);
        }
        mt_tx_service();
        mt_time_service(now);
    }

    mt_emit_pending_events();
    return 0;
}

esp_err_t airngin_transport_destroy(void)
{
    /* esp_mqtt_client_destroy() performs its own stop of a running client, so a
     * stop error here is reported but does not prevent the (authoritative)
     * destroy. Safe to call repeatedly. */
    const esp_err_t db_err = airngin_database_shutdown();    /* cancels an in-flight Cloud Database request */
    if (db_err != ESP_OK) return db_err;
    const esp_err_t stop_err = airngin_transport_stop();
    if (mt_client) {
        const esp_err_t err = esp_mqtt_client_destroy(mt_client);
        if (err != ESP_OK) return err;
        mt_client = NULL;
        mt_clear_effective_profile();
        if (stop_err != ESP_OK) {
            ESP_LOGW(TAG, "MQTT client destroyed after stop error: %s", esp_err_to_name(stop_err));
        }
    }
    if (mt_queue) {
        vQueueDelete(mt_queue);
        mt_queue = NULL;
    }
    /* Close the public TX path: no new producer may start, and those already past the ready
     * check finish (they are short, non-blocking sections) before the queue disappears. */
    portENTER_CRITICAL(&mt_lock);
    mt_tx_accepting = false;
    portEXIT_CRITICAL(&mt_lock);
    for (;;) {
        portENTER_CRITICAL(&mt_lock);
        const unsigned inflight = mt_tx_inflight;
        portEXIT_CRITICAL(&mt_lock);
        if (!inflight) break;
        vTaskDelay(1);
    }
    if (mt_tx_queue) {
        mt_tx_drain();
        vQueueDelete(mt_tx_queue);
        mt_tx_queue = NULL;
    }
    portENTER_CRITICAL(&mt_lock);
    mt_started = false;
    mt_running = false;
    mt_failed = false;
    mt_rejected = false;
    mt_notice_ready = false;
    mt_notice_disconnected = false;
    mt_notice_rejected = false;
    mt_stop_failures = 0;
    mt_start_gate = false;
    mt_next = 0;
    portEXIT_CRITICAL(&mt_lock);
#if CONFIG_AIRNGIN_USE_AES
    mt_aes_wipe();
#endif
    mt_clear_effective_profile();
    return ESP_OK;
}

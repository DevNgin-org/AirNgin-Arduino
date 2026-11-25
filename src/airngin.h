#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "AirNginConfig.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version of the airngin_config_t layout understood by this header. */
#define AIRNGIN_API_VERSION                   3U

/** Upper bound accepted for airngin_config_t.external_reconnect_delay_ms. */
#define AIRNGIN_EXTERNAL_RECONNECT_DELAY_MAX_MS 60000U

#define AIRNGIN_SERIAL_LENGTH                 14U
#define AIRNGIN_PRODUCER_LENGTH               4U
#define AIRNGIN_COMMAND_KEY_MAX               64U
#define AIRNGIN_COMMAND_VALUE_MAX             512U
/** AirNgin Cloud Database key: 1..20 characters, English letters and digits only (server rule). */
#define AIRNGIN_DATABASE_KEY_MAX              20U
/** SDK limit (memory safety), NOT an AirNgin server limit: longest notification message code in bytes. */
#define AIRNGIN_NOTIFICATION_CODE_MAX         64U
#define AIRNGIN_PROVISIONING_LABEL_MAX        16U
#define AIRNGIN_PROVISIONING_PASSWORD_MAX     63U

/* Limits of the cloud (MQTT) credential fields; identical to the stored-profile limits. */
#define AIRNGIN_CLOUD_PROJECT_CODE_MAX        20U
#define AIRNGIN_CLOUD_BROKER_HOST_MAX         100U
#define AIRNGIN_CLOUD_USERNAME_MAX            16U
#define AIRNGIN_CLOUD_PASSWORD_MAX            16U
#define AIRNGIN_CLOUD_CLIENT_ID_MAX           38U

/**
 * AirNgin supports exactly one process-wide instance.
 *
 * Who owns what:
 *
 *   AIRNGIN_NETWORK_MANAGED_WIFI (default; behaves exactly like 1.1.0)
 *     AirNgin owns the Wi-Fi driver, the default STA netif ("WIFI_STA_DEF") and AP netif
 *     ("WIFI_AP_DEF"), its SNTP session, the provisioning SoftAP/HTTP panel and Wi-Fi reconnects.
 *     The host must not initialize any of these before airngin_start(); airngin_start() fails
 *     with ESP_ERR_INVALID_STATE (it never aborts) if the Wi-Fi driver or a default netif already
 *     exists. Cloud credentials come from the profile stored in NVS (written by provisioning).
 *     NVS, esp-netif core and the default event loop are SHARED services: AirNgin initializes them
 *     if needed and never deinitializes, erases or deletes them. In particular AirNgin NEVER erases
 *     the default NVS partition.
 *
 *   AIRNGIN_NETWORK_EXTERNAL
 *     The HOST owns the network (Wi-Fi, Ethernet, PPP, ...), the netifs and time acquisition.
 *     AirNgin never calls any esp_wifi_*, default-Wi-Fi-netif, SNTP, HTTP-server or NVS API and
 *     works without WIFI_STA_DEF / WIFI_AP_DEF. The host tells AirNgin when IP connectivity is
 *     usable with airngin_external_network_set_connected() and keeps the system clock correct.
 *     AirNgin still owns MQTT, TLS, the AirNgin protocol, its queues and callbacks.
 *     Cloud credentials come from airngin_config_t.cloud_credentials. There is no provisioning
 *     panel; airngin_request_provisioning() returns ESP_ERR_NOT_SUPPORTED.
 */
typedef enum {
    AIRNGIN_NETWORK_MANAGED_WIFI = 0,
    AIRNGIN_NETWORK_EXTERNAL = 1,
} airngin_network_mode_t;

/** Deprecated 1.0/1.1 spelling of AIRNGIN_NETWORK_MANAGED_WIFI. */
#define AIRNGIN_NETWORK_MANAGED AIRNGIN_NETWORK_MANAGED_WIFI

/** Where a network event came from. */
typedef enum {
    AIRNGIN_NETWORK_SOURCE_MANAGED_WIFI = 0,
    AIRNGIN_NETWORK_SOURCE_EXTERNAL,
} airngin_network_source_t;

/**
 * The cloud (MQTT) identity AirNgin needs in EXTERNAL mode - exactly the five values the
 * existing transport reads from the stored profile. The broker port (8883), TLS, root CA,
 * MQTT version and topics are fixed by the AirNgin protocol and are not configurable.
 * All strings are COPIED by airngin_init(); the caller may release them afterwards.
 * They are never logged and never appear in status or event data.
 */
typedef struct {
    /** Project code, 1..AIRNGIN_CLOUD_PROJECT_CODE_MAX bytes; no '/', '+', '#'. */
    const char *project_code;
    /** Broker host name, 1..AIRNGIN_CLOUD_BROKER_HOST_MAX bytes (DNS labels). */
    const char *broker_host;
    /** MQTT user name, 1..AIRNGIN_CLOUD_USERNAME_MAX bytes. */
    const char *username;
    /** MQTT password, 1..AIRNGIN_CLOUD_PASSWORD_MAX bytes. */
    const char *password;
    /** MQTT client id, 1..AIRNGIN_CLOUD_CLIENT_ID_MAX bytes. */
    const char *client_id;
    /**
     * Base64 text of exactly 32 bytes: the AES-256-GCM key ("encryptionKey"). REQUIRED when the firmware is built with
     * CONFIG_AIRNGIN_USE_AES=y (invalid or NULL -> airngin_init()/airngin_external_set_cloud_credentials() refuse
     * the credentials; there is no plaintext fallback). IGNORED in PLAIN builds, so it may be left NULL. Never logged.
     * Source-compatible: designated initializers that omit it leave it NULL.
     */
    const char *encryption_key;
} airngin_cloud_credentials_t;

/**
 * Lifecycle states.
 *
 *   UNINITIALIZED --init--> INITIALIZED --start--> STARTING --> RUNNING
 *   RUNNING --stop--> STOPPING --(success)--> INITIALIZED --deinit--> UNINITIALIZED
 *
 * STOPPING is also the state after airngin_stop() returned an error
 * (e.g. ESP_ERR_TIMEOUT): resources may still be live, and airngin_stop()
 * may be called again to finish the shutdown.
 */
typedef enum {
    AIRNGIN_STATE_UNINITIALIZED = 0,
    AIRNGIN_STATE_INITIALIZED,
    AIRNGIN_STATE_STARTING,
    AIRNGIN_STATE_RUNNING,
    AIRNGIN_STATE_STOPPING,
} airngin_state_t;

/**
 * Events. Ordering guarantees (see airngin_event_handler_t for contexts):
 *   STARTED precedes every other event of a run; STOPPED is last and only after a complete shutdown.
 *   PROVISIONING_STARTED precedes PROVISIONING_{SUCCESS,TIMEOUT,FAILED(open)} of the same panel session.
 *   PROVISIONING_TIMEOUT and PROVISIONING_CANCELLED are each immediately followed by PROVISIONING_STOPPED;
 *   a session produces exactly one of SUCCESS / TIMEOUT / CANCELLED. When they race, SUCCESS wins.
 *   PROVISIONING_SUCCESS is not followed by STOPPED: the device restarts after a short grace period.
 *   MQTT_READY / MQTT_DISCONNECTED alternate; MQTT_DISCONNECTED is delivered before CREDENTIALS_REJECTED.
 */
/** Which AirNgin time feed to follow (server broadcast about every 5 minutes). */
typedef enum {
    AIRNGIN_TIME_SOURCE_UTC = 0,       /**< `<projectCode>/Time`        */
    AIRNGIN_TIME_SOURCE_TEHRAN,        /**< `<projectCode>/Time/Tehran` */
} airngin_time_source_t;

/**
 * One parsed AirNgin time packet. Calendar values are exactly what the server sent (the SDK does not convert
 * between calendars and does not interpret day_of_week / week_number conventions beyond structural range checks).
 * `received_at_ms` is the device's own monotonic clock (esp_timer) at reception: it is NOT server time.
 */
typedef struct {
    airngin_time_source_t source;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t day_of_week;
    uint16_t day_of_year;
    uint8_t week_number;
    int64_t received_at_ms;
} airngin_time_info_t;

/** Notification channel for airngin_send_notification_to_user(). Values are the AirNgin documented ones. */
typedef enum {
    AIRNGIN_NOTIFICATION_SMS  = 20,
    AIRNGIN_NOTIFICATION_PUSH = 30,
} airngin_notification_channel_t;

/** Cloud Database operations that exist in the AirNgin API (save/update, read, delete). There is no list/query call. */
typedef enum {
    AIRNGIN_DATABASE_OP_SET = 1,       /**< AddNewData: creates the key or UPDATES its value */
    AIRNGIN_DATABASE_OP_GET,           /**< GetDeviceCloud */
    AIRNGIN_DATABASE_OP_DELETE,        /**< DeleteData */
} airngin_database_operation_t;

/**
 * Result of one Cloud Database request (AIRNGIN_EVENT_DATABASE_RESULT). All strings are owned by the SDK and valid ONLY
 * during the event callback: copy what you need.
 *   status: ESP_OK (server isSuccess=true), ESP_FAIL (server answered isSuccess=false: see `message`),
 *           ESP_ERR_TIMEOUT, ESP_ERR_INVALID_RESPONSE (HTTP status != 200 or unusable body), ESP_ERR_NO_MEM (response too
 *           large / allocation), ESP_ERR_INVALID_STATE (SDK stopped / network or clock lost), or the esp-tls / esp-http error.
 *   An error or timeout does NOT say whether a SET/DELETE reached the server: read the key to find out. Nothing is retried.
 */
typedef struct {
    airngin_database_operation_t operation;
    uint32_t request_id;               /**< the value returned by the API call; local to the SDK, never sent to the server */
    esp_err_t status;
    int http_status;                   /**< 0 when no HTTP response was received */
    bool server_success;               /**< the server's isSuccess */
    const char *key;
    const char *value;                 /**< GET + success only: the stored string (opaque, may be JSON), else NULL */
    size_t value_len;
    const char *message;               /**< server message text (language of the server, may be empty) */
} airngin_database_event_t;

typedef enum {
    AIRNGIN_EVENT_STARTED = 0,
    AIRNGIN_EVENT_STOPPED,
    /** Provisioning panel opened (initial, credential recovery or airngin_request_provisioning()). */
    AIRNGIN_EVENT_PROVISIONING_STARTED,
    /** Panel closed without a new configuration (follows TIMEOUT). */
    AIRNGIN_EVENT_PROVISIONING_STOPPED,
    /** A new configuration was validated, saved and read back. The device restarts shortly after. */
    AIRNGIN_EVENT_PROVISIONING_SUCCESS,
    /** The 120 s panel session expired without a new configuration. */
    AIRNGIN_EVENT_PROVISIONING_TIMEOUT,
    /** The panel could not be opened; `error` holds the esp_err_t. */
    AIRNGIN_EVENT_PROVISIONING_FAILED,
    /** IP connectivity became usable (managed Wi-Fi got an IP, or the host reported it). */
    AIRNGIN_EVENT_NETWORK_CONNECTED,
    /** IP connectivity was lost. In EXTERNAL mode it may be delivered before MQTT_DISCONNECTED. */
    AIRNGIN_EVENT_NETWORK_DISCONNECTED,
    /** MQTT connected AND the AirNgin subscription was acknowledged (the "ready" transport state). */
    AIRNGIN_EVENT_MQTT_READY,
    AIRNGIN_EVENT_MQTT_DISCONNECTED,
    AIRNGIN_EVENT_CREDENTIALS_REJECTED,
    /** An active provisioning session was explicitly closed by airngin_cancel_provisioning(), before success or timeout.
     *  Always followed by PROVISIONING_STOPPED (never by TIMEOUT). */
    AIRNGIN_EVENT_PROVISIONING_CANCELLED,
    /** A valid AirNgin time packet arrived (see airngin_time_subscribe()). `data.time` holds it. Owner task. */
    AIRNGIN_EVENT_TIME_RECEIVED,
    /** A Cloud Database request finished (any outcome). `data.database`. Owner task. */
    AIRNGIN_EVENT_DATABASE_RESULT,
} airngin_event_t;

/** Deprecated 1.1.0 names of the generic network events. */
#define AIRNGIN_EVENT_WIFI_CONNECTED    AIRNGIN_EVENT_NETWORK_CONNECTED
#define AIRNGIN_EVENT_WIFI_DISCONNECTED AIRNGIN_EVENT_NETWORK_DISCONNECTED

/** Alias: the MQTT "connected" event of AirNgin means connected + subscribed. */
#define AIRNGIN_EVENT_MQTT_CONNECTED AIRNGIN_EVENT_MQTT_READY

/** Stable AirNgin-level event description; no ESP-IDF internal structures are exposed. */
typedef struct {
    airngin_event_t type;
    /** esp_err_t associated with the event, ESP_OK when not applicable. */
    esp_err_t error;
    union {
        /** NETWORK_CONNECTED / NETWORK_DISCONNECTED.
         *  reason: managed Wi-Fi: the last disconnect reason code (0 = none); external: always 0. */
        struct { airngin_network_source_t source; int reason; } network;
        /** MQTT_READY / MQTT_DISCONNECTED / CREDENTIALS_REJECTED.
         *  reason: MQTT CONNACK return code if refused, else an MQTT error-type code (0 = none).
         *  tx_discarded: queued, unsent publishes discarded because the session ended. */
        struct { int reason; uint32_t tx_discarded; } mqtt;
        /** TIME_RECEIVED. */
        airngin_time_info_t time;
        /** DATABASE_RESULT. Pointers are valid only during the callback. */
        airngin_database_event_t database;
    } data;
} airngin_event_data_t;

typedef struct {
    char key[AIRNGIN_COMMAND_KEY_MAX + 1U];
    char value[AIRNGIN_COMMAND_VALUE_MAX + 1U];
} airngin_command_t;

typedef struct {
    bool has_value;
    /** Optional response key. When has_key is false the request key is echoed. */
    bool has_key;
    char key[AIRNGIN_COMMAND_KEY_MAX + 1U];
    char value[AIRNGIN_COMMAND_VALUE_MAX + 1U];
} airngin_command_result_t;

/**
 * Command callback.
 *
 * Context: runs synchronously on AirNgin's owner task ("airngin_owner"), never
 * from the ESP-MQTT event callback and never while an AirNgin spinlock is held.
 * Commands are executed one at a time, in arrival order.
 *
 * `command` and `result` are owned by AirNgin and valid only for the duration
 * of the callback. The callback should return promptly: while it runs the
 * owner task does not service Wi-Fi/MQTT/provisioning timers.
 *
 * FORBIDDEN: the callback MUST NOT call airngin_stop() or airngin_deinit().
 * Both would wait for the owner task to terminate, i.e. for themselves; they
 * therefore fail immediately with ESP_ERR_INVALID_STATE when called from the
 * owner task. Schedule such requests on an application task instead.
 * airngin_get_state() is safe.
 */
typedef esp_err_t (*airngin_command_handler_t)(
    const airngin_command_t *command,
    airngin_command_result_t *result,
    void *user_ctx);

/**
 * Event callback. `event` is valid only for the duration of the call.
 *
 * Context:
 *   - STARTED: the task that called airngin_start(), before the owner task is
 *     released, so STARTED is always delivered before any runtime event.
 *     airngin_get_state() reports STARTING at that moment.
 *   - STOPPED: the task that called airngin_stop(), after shutdown completed.
 *   - every other event: AirNgin's owner task.
 * Runtime events (owner task) and lifecycle events (caller task) may therefore
 * run on different tasks; the handler must be safe against that. Callbacks are
 * never invoked while an AirNgin spinlock is held.
 *
 * Allowed from an event callback: airngin_get_state(), airngin_get_status(),
 * airngin_is_ready(), airngin_publish(), airngin_report(),
 * airngin_request_provisioning() (all non-blocking).
 * Forbidden: airngin_stop()/airngin_deinit() on the owner task (they fail with
 * ESP_ERR_INVALID_STATE), and airngin_start()/airngin_stop() from STARTED
 * (state is STARTING). From STOPPED (state INITIALIZED) airngin_start()/
 * airngin_deinit() are permitted. Keep callbacks short: the owner task does not
 * service Wi-Fi/MQTT while one runs.
 */
typedef void (*airngin_event_handler_t)(const airngin_event_data_t *event, void *user_ctx);

/**
 * Configuration. Always obtain it from AIRNGIN_CONFIG_DEFAULT() and then modify
 * fields; struct_size and api_version are filled in there. Fields added in
 * later API versions are appended at the end, and airngin_init() treats a
 * config whose struct_size does not cover them as "use defaults".
 *
 * Lifetime:
 *   - serial, producer_code, provisioning_label and provisioning_password are
 *     COPIED by airngin_init(); the caller may free or modify them afterwards.
 *   - command_handler, event_handler and user_ctx are stored as given.
 *     user_ctx (and the code of both handlers) MUST remain valid until
 *     airngin_stop() has returned ESP_OK. If airngin_stop() returned an error
 *     and the state is still STOPPING, the owner task may still call the
 *     handlers, so they must stay valid until a later airngin_stop() succeeds.
 */
typedef struct {
    /** sizeof(airngin_config_t) as seen by the caller. */
    size_t struct_size;
    /** AIRNGIN_API_VERSION the caller was written against. */
    uint32_t api_version;
    /** 14 characters: 4-character producer code followed by 10 decimal digits. */
    const char *serial;
    /** Exactly 4 ASCII alpha-numeric characters; must match serial prefix. */
    const char *producer_code;
    /** AIRNGIN_NETWORK_MANAGED_WIFI (default) or AIRNGIN_NETWORK_EXTERNAL. */
    airngin_network_mode_t network_mode;
    /** MANAGED_WIFI only (ignored in EXTERNAL): provisioning SSID is <serial>_<label>. */
    const char *provisioning_label;
    /** MANAGED_WIFI only (ignored in EXTERNAL): WPA2 provisioning AP password, 8..63 printable ASCII. */
    const char *provisioning_password;
    /** Optional application command handler. Without it commands are ignored. */
    airngin_command_handler_t command_handler;
    /** Optional lifecycle/network event observer. */
    airngin_event_handler_t event_handler;
    /** Passed unchanged to both callbacks. See the lifetime rules above. */
    void *user_ctx;
    /* ---- api_version >= 2 ---------------------------------------------------------- */
    /**
     * EXTERNAL mode: REQUIRED, copied by airngin_init(). MANAGED_WIFI mode: must be NULL
     * (credentials come from the provisioned profile); a non-NULL value is rejected.
     */
    const airngin_cloud_credentials_t *cloud_credentials;
    /* ---- api_version >= 3 ---------------------------------------------------------- */
    /**
     * EXTERNAL mode only (ignored in MANAGED_WIFI, whose 60 s restoration delay is unchanged):
     * how long after the host reports "network up" - and after a credential replacement - AirNgin
     * waits before starting TLS/MQTT. It only avoids starting TLS in the same instant as the
     * notification; it is not a network-stabilization delay. 0 .. AIRNGIN_EXTERNAL_RECONNECT_DELAY_MAX_MS.
     * AIRNGIN_CONFIG_DEFAULT() uses CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS (3000). A broker failure while the
     * network is up keeps the normal 60 s MQTT back-off.
     */
    uint32_t external_reconnect_delay_ms;
} airngin_config_t;

/** Return the build-time defaults configured through Kconfig. */
airngin_config_t airngin_config_default(void);
#define AIRNGIN_CONFIG_DEFAULT() airngin_config_default()

/**
 * Copy and validate configuration. Does not start networking or tasks.
 * NULL selects airngin_config_default().
 * ESP_ERR_INVALID_SIZE: struct_size too small; ESP_ERR_NOT_SUPPORTED: unknown api_version,
 * or MANAGED_WIFI requested while the component is built without it; ESP_ERR_INVALID_ARG:
 * bad identity, label/password (managed), unknown network_mode, missing or invalid
 * cloud_credentials (external), or cloud_credentials supplied in managed mode;
 * ESP_ERR_INVALID_STATE: already initialized.
 * A config built for api_version 1 (smaller struct) is still accepted: newer fields default to NULL.
 */
esp_err_t airngin_init(const airngin_config_t *config);

/**
 * Start the managed AirNgin Wi-Fi/provisioning/MQTT runtime.
 * Requires INITIALIZED. On failure the state returns to INITIALIZED and
 * start may be retried. ESP_ERR_INVALID_STATE is also returned when the host
 * already owns the Wi-Fi driver or a default Wi-Fi netif.
 */
esp_err_t airngin_start(void);

/**
 * Stop the owner task and release all AirNgin-owned Wi-Fi/MQTT/provisioning
 * resources. Valid in RUNNING and STOPPING.
 * ESP_OK: fully stopped, state INITIALIZED, STOPPED delivered.
 * ESP_ERR_TIMEOUT or another error: shutdown is incomplete, state stays
 * STOPPING and NO STOPPED event is delivered; call airngin_stop() again.
 * ESP_ERR_INVALID_STATE: wrong state, a concurrent stop is in progress, or
 * called from the owner task (forbidden, see airngin_command_handler_t).
 */
esp_err_t airngin_stop(void);

/**
 * Release the singleton configuration. Requires INITIALIZED (stop first).
 * Fails with ESP_ERR_INVALID_STATE from the owner task.
 */
esp_err_t airngin_deinit(void);

/** Return a snapshot of the singleton lifecycle state. Safe from any task. */
airngin_state_t airngin_get_state(void);

/**
 * Lightweight snapshot of the SDK. Initialize `struct_size = sizeof(airngin_status_t)`
 * (or use AIRNGIN_STATUS_INIT) so the structure can grow in later versions.
 * Non-blocking, no network access. Every field is read atomically on its own; the set
 * is a best-effort snapshot, and `ready` is derived from the other fields of the same snapshot.
 * (1.2.0: `wifi_connected` became `network_connected`, `provisioned` became `credentials_available`.)
 */
typedef struct {
    size_t struct_size;
    /** Same value as airngin_get_state(). */
    airngin_state_t lifecycle;
    /** The configured network mode (AIRNGIN_NETWORK_MANAGED_WIFI before airngin_init()). */
    airngin_network_mode_t network_mode;
    /**
     * Usable IP connectivity. Managed: the STA holds an IP (false in provisioning-AP mode).
     * External: the state last reported with airngin_external_network_set_connected().
     */
    bool network_connected;
    /** MQTT session established (CONNACK received). Subscription may still be pending. */
    bool mqtt_connected;
    /** MQTT connected AND the AirNgin command-topic subscription (SUBACK) acknowledged. */
    bool mqtt_ready;
    /**
     * Cloud credentials are available. Managed: a valid profile is stored in NVS.
     * External: always true after airngin_init() (they were validated there).
     */
    bool credentials_available;
    /** The provisioning panel (SoftAP + HTTP) is currently open. Always false in EXTERNAL mode. */
    bool provisioning_active;
    /** Trusted system clock (required before any TLS MQTT connection). */
    bool clock_trusted;
    /**
     * READY: lifecycle == RUNNING && network_connected && clock_trusted && mqtt_ready.
     * Identical in both network modes. Only then are airngin_publish()/airngin_report() accepted.
     */
    bool ready;
    /** Complete inbound commands dropped because the command queue was full. */
    uint32_t rx_dropped;
    /** Public publishes refused because the TX queue was full or memory was short. */
    uint32_t tx_rejected;
    /** Queued publishes that could not be sent (serialize/publish error). */
    uint32_t tx_failed;
    /**
     * The broker rejected the current cloud credentials (CONNACK 4/5) and AirNgin is suspended.
     * External: cleared by a valid airngin_external_set_cloud_credentials() and by the next MQTT_READY.
     * Managed: true while the device is in credential-recovery mode.
     */
    bool credentials_rejected;
} airngin_status_t;

#define AIRNGIN_STATUS_INIT { .struct_size = sizeof(airngin_status_t) }

/** Fill `status`. ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_SIZE if struct_size is too small. */
esp_err_t airngin_get_status(airngin_status_t *status);

/** True when airngin_get_status().ready is true. */
bool airngin_is_ready(void);

/**
 * Publish a key/value pair to AirNgin (device -> server) using the same
 * `[{"key":..,"value":..}]` envelope and the same outbound topic
 * (`<projectCode>/DeviceToServer`, QoS 1) as command responses.
 *
 * Semantics:
 *   - key 1..AIRNGIN_COMMAND_KEY_MAX bytes, value 0..AIRNGIN_COMMAND_VALUE_MAX bytes,
 *     both valid UTF-8 (a C string cannot carry an embedded NUL).
 *   - The strings are COPIED before this function returns: the caller may free or
 *     modify them immediately. Memory is bounded: one heap block of at most
 *     AIRNGIN_COMMAND_KEY_MAX + AIRNGIN_COMMAND_VALUE_MAX + 14 bytes per queued message and
 *     CONFIG_AIRNGIN_TX_QUEUE_DEPTH messages at most.
 *   - Never blocks: it only enqueues. The owner task transmits. Success means
 *     "accepted for transmission", not "delivered".
 *   - Accepted only when airngin_get_status().ready is true; otherwise
 *     ESP_ERR_INVALID_STATE. There is no offline buffering.
 *   - ESP_ERR_NO_MEM: TX queue full or allocation failed (counted in tx_rejected).
 *   - Queued but unsent messages are DISCARDED when the MQTT session ends (reported through
 *     airngin_event_data_t.data.mqtt.tx_discarded); they are never delivered after a reconnect.
 *   - Safe from any task, including the AirNgin command and event callbacks.
 *     Not for ISRs. Do not call from inside the ESP-MQTT event callback.
 */
esp_err_t airngin_publish(const char *key, const char *value);

/**
 * Report device state (unsolicited). Today this has exactly the wire behaviour of
 * airngin_publish() (the server protocol has one device->server topic); the separate
 * name lets future protocol versions distinguish reported state from telemetry
 * without changing application code. Same rules and error codes as airngin_publish().
 */
esp_err_t airngin_report(const char *key, const char *value);

/**
 * Ask AirNgin to open its provisioning panel without a reboot. Asynchronous and
 * non-blocking: it only records the request; the owner task stops MQTT, then opens
 * the SoftAP + HTTP panel (event PROVISIONING_STARTED, or PROVISIONING_FAILED).
 *
 *   - ESP_ERR_NOT_SUPPORTED in EXTERNAL mode (AirNgin never takes over the host network).
 *   - ESP_ERR_INVALID_STATE unless the state is RUNNING.
 *   - Repeated requests, or a request while the panel is already open or a restart
 *     is pending, are coalesced and return ESP_OK; they never extend the 120 s session.
 *   - The stored profile is never erased by this call. If no new valid profile is saved
 *     before the session times out, AirNgin resumes station mode with the stored profile
 *     (MQTT reconnects after the usual 60 s restoration delay). A device that has no valid
 *     profile, or is in credential recovery, simply gets its panel (re)opened.
 *   - A successful provisioning still saves, waits the 3 s grace period and restarts.
 *   - Safe from any task, including the command and event callbacks.
 */
esp_err_t airngin_request_provisioning(void);

/**
 * EXTERNAL mode only: tell AirNgin whether the host network currently provides usable IP
 * connectivity. Call it when the host gets/loses an IP (Wi-Fi, Ethernet, PPP, ...), and once
 * before airngin_start() if the network is already up.
 *
 *   - ESP_ERR_NOT_SUPPORTED unless configured with AIRNGIN_NETWORK_EXTERNAL.
 *   - ESP_ERR_INVALID_STATE before airngin_init(). Accepted in INITIALIZED, STARTING, RUNNING and
 *     STOPPING; the last reported value survives airngin_stop()/airngin_start(). airngin_deinit()
 *     forgets it (the host reports again after the next init).
 *   - Idempotent: repeating the current value changes nothing. Real changes are counted so that a
 *     quick down/up pair is still seen by the owner task and forces a fresh MQTT session.
 *   - Non-blocking: only updates a flag. The owner task (within ~100 ms) performs the transition,
 *     emits NETWORK_CONNECTED/DISCONNECTED and starts/stops MQTT. It never calls user callbacks
 *     and never touches the host network. `ready` and publish react immediately.
 *   - Safe from any FreeRTOS task and from ESP-IDF event-loop callbacks. NOT ISR safe.
 *   - Going down loses command responses still in flight and discards queued unsent publishes
 *     (see airngin_publish()). After coming back, MQTT starts after the EXTERNAL reconnect delay
 *     (airngin_config_t.external_reconnect_delay_ms, default 3 s) - not the 60 s managed-Wi-Fi delay - and
 *     once the clock is trusted. A newer "up" replaces the hold of an older one, so a reconnect scheduled
 *     for an earlier "up" can never start a stale session.
 */
esp_err_t airngin_external_network_set_connected(bool connected);

/**
 * Close an active managed provisioning session without waiting for the 120 s timeout (MANAGED_WIFI mode only).
 * The mirror of airngin_request_provisioning(): asynchronous, non-blocking, coalescing - it only records the request;
 * the owner task closes the HTTP server and SoftAP, emits PROVISIONING_CANCELLED then PROVISIONING_STOPPED and, if a
 * valid profile was stored when the panel opened, restores station mode with it (MQTT then returns through the normal
 * managed recovery: after the IP is back, plus the 60 s restoration delay; the MQTT start gate stays shut until then).
 * Without a stored profile the device stays RUNNING and idle (network down, provisioning inactive); the panel reopens
 * only on a new airngin_request_provisioning() or a restart - it is never reopened automatically after a cancel.
 *
 *   - ESP_ERR_NOT_SUPPORTED in EXTERNAL mode (the host network is never touched).
 *   - ESP_ERR_INVALID_STATE unless RUNNING, or when no provisioning is active/opening (nothing is pretended), or when a
 *     configuration has already been committed (provisioning succeeded; the restart is pending): SUCCESS WINS and the
 *     new profile is never rolled back.
 *   - A pending, not yet opened request is simply dropped. Repeated calls are coalesced. Cancel vs timeout: whichever the
 *     owner task sees first closes the session, exactly once.
 *   - Safe from any task, including the command and event callbacks.
 */
esp_err_t airngin_cancel_provisioning(void);

/**
 * EXTERNAL mode only: replace the cloud credentials at run time - for example after
 * AIRNGIN_EVENT_CREDENTIALS_REJECTED, when the project, broker or client identity changes, or when the
 * password is rotated - without stopping AirNgin and without a fake network edge.
 *
 *   - Same fields, limits and validation as airngin_config_t.cloud_credentials (ESP_ERR_INVALID_ARG on
 *     any violation; the previous credentials stay in force).
 *   - ESP_ERR_NOT_SUPPORTED in AIRNGIN_NETWORK_MANAGED_WIFI mode (the stored profile is never modified).
 *     ESP_ERR_INVALID_STATE in UNINITIALIZED and STOPPING. Accepted in INITIALIZED, STARTING and RUNNING.
 *   - Non-blocking: validate, COPY (the caller may release its strings immediately), wipe the previous copy,
 *     bump an internal credential generation, return. No MQTT/TLS/network work and no user callback happens
 *     inside the call. The owner task (within ~100 ms) ends the old MQTT session, discards queued unsent
 *     publishes (they were accepted under the old identity; counted in tx_discarded of MQTT_DISCONNECTED),
 *     rebuilds topics (new project_code), recreates the MQTT client (new broker_host, username, password,
 *     client_id) and connects with the usual external reconnect delay - if the network is up and the clock is
 *     trusted. While the replacement is pending `ready`, `mqtt_ready` and airngin_publish() already refuse.
 *   - If the network is down or the clock untrusted nothing is attempted until that changes.
 *   - Several quick calls: only the latest valid credentials are ever used. A session started with an older
 *     generation can never become READY once a newer one was installed.
 *   - After CREDENTIALS_REJECTED AirNgin stays suspended (no retry storm) until this function installs new
 *     credentials or the lifecycle is restarted. Counters rx_dropped / tx_rejected / tx_failed are kept.
 *   - Credentials live in RAM only (never written to NVS); they survive airngin_stop()/airngin_start() and are
 *     wiped by airngin_deinit(). Never logged, never in status or events.
 *   - Safe from any FreeRTOS task, ESP-IDF event-loop callbacks and AirNgin's own event and command callbacks.
 *     NOT ISR safe.
 */
esp_err_t airngin_external_set_cloud_credentials(const airngin_cloud_credentials_t *credentials);

/**
 * Follow an AirNgin time feed. This is a SUBSCRIPTION, not a request: the server broadcasts about every 5 minutes, so
 * the first packet can take that long. Non-blocking; only records the wish - the owner task (un)subscribes.
 *   - Allowed in INITIALIZED, STARTING and RUNNING (so the source can be chosen before MQTT is ready); otherwise
 *     ESP_ERR_INVALID_STATE. ESP_ERR_INVALID_ARG for an unknown source.
 *   - Re-applied automatically after every MQTT reconnect; the wish is kept across airngin_stop()/airngin_start()
 *     and forgotten by airngin_deinit().
 *   - Switching UTC <-> Tehran unsubscribes the old topic and then subscribes the new one; only one feed is active.
 *   - The time topics are public AirNgin topics: they never carry the `/AES` suffix, also in AES builds (MQTT itself
 *     is still TLS). They do not affect `ready`: MQTT_READY still means "the personal command topic is subscribed".
 *   - Each valid packet updates airngin_time_get_latest() and raises AIRNGIN_EVENT_TIME_RECEIVED.
 *   - This feed is NOT the TLS bootstrap clock (that needs a trusted system clock before MQTT can connect at all).
 */
esp_err_t airngin_time_subscribe(airngin_time_source_t source);

/** Stop following the time feed (same states as airngin_time_subscribe(); idempotent). The latest packet is kept. */
esp_err_t airngin_time_unsubscribe(void);

/**
 * Copy the latest valid time packet. ESP_ERR_NOT_FOUND until one was received, ESP_ERR_INVALID_STATE when
 * UNINITIALIZED, ESP_ERR_INVALID_ARG for NULL. Thread safe; the result is a plain copy.
 */
esp_err_t airngin_time_get_latest(airngin_time_info_t *out);

/**
 * AirNgin Cloud Database (optional, off by default: CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE). It is NOT an MQTT feature:
 * AirNgin documents it as an HTTPS API (`https://app.airngin.com/api/v1.0/DeviceCloud/...`) authenticated with the
 * device's own MQTT user name and password, serial and project code. The SDK performs it on a separate worker task
 * with certificate-verified TLS, so neither your task nor AirNgin's MQTT owner task ever waits for the network.
 *
 * COST: the server charges Cloud Database use to the project owner's wallet and refuses requests without credit. Nothing is
 * ever sent by the SDK on its own, and no repository example calls these functions.
 * Records: the documentation says up to 5 per device (the server enforces it). A SET of an existing key updates it.
 *
 * Common rules for set / get / delete:
 *   - Non-blocking. The strings are COPIED before the call returns. `request_id` (optional) identifies the result event.
 *   - ONE request at a time: until its AIRNGIN_EVENT_DATABASE_RESULT was delivered, the next call returns ESP_ERR_NO_MEM.
 *   - key: 1..AIRNGIN_DATABASE_KEY_MAX characters, letters and digits only (ESP_ERR_INVALID_SIZE / ESP_ERR_INVALID_ARG).
 *   - ESP_ERR_INVALID_STATE: not RUNNING, no network, clock not trusted (TLS), no credentials yet (managed mode before
 *     provisioning), or the project code / serial do not have the server's required lengths (16 / 14).
 *     MQTT readiness is NOT required, and the Cloud Database never affects `ready` or MQTT.
 *   - ESP_ERR_NOT_SUPPORTED: feature compiled out, or an AES build without CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD
 *     (the Cloud Database has no application-layer AES form; values and credentials would travel over TLS only).
 *   - ESP_OK means "accepted"; the outcome arrives in AIRNGIN_EVENT_DATABASE_RESULT. Writes are never retried.
 */
esp_err_t airngin_database_set(const char *key, const char *value, uint32_t *request_id);
esp_err_t airngin_database_get(const char *key, uint32_t *request_id);
esp_err_t airngin_database_delete(const char *key, uint32_t *request_id);

/**
 * Ask AirNgin to send a notification to the product owner / center (`<projectCode>/SendNotificationToCenter`).
 * `code` is the message code configured and approved in the AirNgin panel (1..AIRNGIN_NOTIFICATION_CODE_MAX bytes,
 * printable UTF-8, no control characters; the length bound is an SDK limit, not a server limit).
 *
 * COST: AirNgin SMS / notification services can be charged to your AirNgin wallet. The SDK never sends anything by
 * itself and does not check balances; only an explicit call of this function sends.
 *
 * Non-blocking: the code is copied, queued, and published by the owner task (QoS 1, same TX queue as airngin_publish()).
 * ESP_OK means "accepted by the SDK for MQTT transmission" - NOT "SMS delivered", "charged" or "code accepted":
 * the protocol has no acknowledgement.
 *   - ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE: bad code.
 *   - ESP_ERR_INVALID_STATE: not RUNNING or MQTT not ready (no offline buffering, nothing is stored).
 *   - ESP_ERR_NO_MEM: TX queue full.
 *   - ESP_ERR_NOT_SUPPORTED: AES build (CONFIG_AIRNGIN_USE_AES=y) while server support for the `/AES` notification
 *     topics is not confirmed (CONFIG_AIRNGIN_NOTIFICATION_AES_CONFIRMED=n). The SDK never downgrades to plaintext.
 */
esp_err_t airngin_send_notification_to_center(const char *code);

/**
 * Ask AirNgin to notify the device owner / master user (`<projectCode>/SendNotificationToUser`) by SMS (20) or
 * AirNgin push (30). Same rules, costs and error codes as airngin_send_notification_to_center();
 * ESP_ERR_INVALID_ARG also for a `channel` other than the two documented values.
 */
esp_err_t airngin_send_notification_to_user(const char *code, airngin_notification_channel_t channel);

/** Thin wrappers over the two functions above. */
esp_err_t airngin_send_sms_to_center(const char *code);
esp_err_t airngin_send_sms_to_user(const char *code);
esp_err_t airngin_send_push_to_user(const char *code);

/**
 * Set the response value. Safe: the length is validated first and the result is
 * left unchanged on error. Returns ESP_ERR_INVALID_SIZE if value is longer than
 * AIRNGIN_COMMAND_VALUE_MAX.
 */
esp_err_t airngin_command_result_set(airngin_command_result_t *result, const char *value);

/**
 * Optionally override the response key (default: the request key). Key must be
 * 1..AIRNGIN_COMMAND_KEY_MAX bytes. Existing applications that do not call this
 * keep the previous wire output.
 */
esp_err_t airngin_command_result_set_key(airngin_command_result_t *result, const char *key);

#ifdef __cplusplus
}
#endif

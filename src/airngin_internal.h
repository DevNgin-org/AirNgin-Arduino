#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airngin.h"
#include "esp_err.h"
#include "airngin_profile.h"

/* Fixed by the AirNgin protocol (TLS, no configuration). */
#define AIRNGIN_MQTT_PORT 8883U

/* Overwrite sensitive memory in a way the optimizer may not remove. */
static inline void airngin_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* Configuration facts the runtimes need (copied at airngin_init()). */
airngin_network_mode_t airngin_internal_network_mode(void);
/* EXTERNAL mode: copy the validated cloud credentials into the profile layout the transport uses.
 * The caller must airngin_secure_zero() the result. Returns false if not configured. */
bool airngin_internal_cloud_profile(airngin_profile *out, uint32_t *generation);
/* Credential generation: changes with every successful credential replacement. */
uint32_t airngin_internal_cloud_generation(void);
uint32_t airngin_internal_external_reconnect_delay_ms(void);
/* Owner task: record that the broker rejected / accepted credential generation `generation`.
 * Ignored if a newer generation was installed meanwhile. */
void airngin_internal_credentials_rejected(uint32_t generation);
void airngin_internal_credentials_accepted(uint32_t generation);
bool airngin_internal_credentials_rejected_flag(void);
/* EXTERNAL mode: host-reported connectivity and an edge counter (changes on every real up/down). */
void airngin_internal_external_network(bool *connected, uint32_t *generation);

/* Validation/conversion of public cloud credentials (airngin_cloud.c). */
bool airngin_cloud_valid(const airngin_profile *profile);
bool airngin_cloud_to_profile(const airngin_cloud_credentials_t *credentials, airngin_profile *out);

const char *airngin_internal_serial(void);
const char *airngin_internal_producer_code(void);
const char *airngin_internal_provisioning_label(void);
const char *airngin_internal_provisioning_password(void);
esp_err_t airngin_internal_dispatch(const airngin_command_t *command,
                                    airngin_command_result_t *result);
/* Deliver an event (never under a lock). The event_t-only form carries no data. */
void airngin_internal_emit(const airngin_event_data_t *event);
void airngin_internal_emit_event(airngin_event_t event);

/* Snapshot of runtime-side facts for airngin_get_status(). Non-blocking. */
typedef struct {
    bool network_connected;
    bool credentials_available;
    bool provisioning_active;
    bool clock_trusted;
    bool mqtt_connected;
    bool mqtt_ready;
    bool credentials_rejected;
    uint32_t rx_dropped;
    uint32_t tx_rejected;
    uint32_t tx_failed;
} airngin_runtime_status_t;
void airngin_runtime_get_status(airngin_runtime_status_t *out);

/* Internal snapshot of the exact cloud identity used by the current ESP-MQTT client. Database requests take their
 * own copy from this snapshot; there is no independently configured or persisted Database identity. */
typedef enum {
    AIRNGIN_EFFECTIVE_CREDENTIALS_MANAGED_NVS_PROFILE = 0,
    AIRNGIN_EFFECTIVE_CREDENTIALS_EXTERNAL_RUNTIME,
} airngin_effective_credentials_source_t;

typedef struct {
    airngin_effective_credentials_source_t source;
    char project_code[sizeof(((airngin_profile *)0)->projectCode)];
    char username[sizeof(((airngin_profile *)0)->mqttUser)];
    char password[sizeof(((airngin_profile *)0)->mqttPass)];
    char client_id[sizeof(((airngin_profile *)0)->clientId)];
    char broker_host[sizeof(((airngin_profile *)0)->mqttBroker)];
    char device_serial[AIRNGIN_SERIAL_LENGTH + 1U];
} airngin_effective_cloud_credentials_t;

/* ESP_ERR_INVALID_STATE means no current MQTT client identity exists, or an EXTERNAL replacement is not applied yet. */
esp_err_t airngin_runtime_get_effective_cloud_credentials(airngin_effective_cloud_credentials_t *out);
/* Copy and queue a validated pair for the owner task. INVALID_STATE unless transport is ready. */
esp_err_t airngin_runtime_publish(const char *key, size_t key_len, const char *value, size_t value_len);
/* Queue a notification (to_user: SendNotificationToUser with `channel`, else SendNotificationToCenter). Code pre-validated.
 * ESP_ERR_INVALID_STATE unless the transport is ready (and, external mode, the network is up). */
esp_err_t airngin_runtime_notify(bool to_user, int channel, const char *code, size_t code_len);
/* AirNgin time feed: the application's wish (read by the owner task) and the latest packet (written by it). */
bool airngin_internal_time_wanted(airngin_time_source_t *source);
void airngin_internal_time_store(const airngin_time_info_t *info);
/* Record a provisioning request for the owner task. Never blocks. */
esp_err_t airngin_runtime_request_provisioning(void);
/* Record a request to close the active provisioning session. Never blocks. INVALID_STATE if none is active/opening. */
esp_err_t airngin_runtime_cancel_provisioning(void);

/*
 * Runtime (Wi-Fi / provisioning / MQTT / owner task).
 *
 * airngin_runtime_start() creates the owner task GATED: it does nothing until
 * airngin_runtime_release_owner() is called, so lifecycle code can deliver
 * AIRNGIN_EVENT_STARTED first.
 *
 * airngin_runtime_stop() is retryable: ESP_ERR_TIMEOUT (owner has not reached
 * its safe point yet) or a cleanup error leave every remaining resource owned
 * and consistent, and a later call continues where the previous one stopped.
 */
esp_err_t airngin_runtime_start(void);
void airngin_runtime_release_owner(void);
esp_err_t airngin_runtime_stop(void);
bool airngin_runtime_in_owner_task(void);

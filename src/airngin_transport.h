#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airngin_profile.h"
#include "esp_err.h"

esp_err_t airngin_transport_init(const airngin_profile *profile);
/* Copy the exact stable profile used to create the current ESP-MQTT client. Internal credential consumers only. */
esp_err_t airngin_transport_get_effective_cloud_profile(airngin_profile *out);
int airngin_transport_tick(bool ip, int64_t now);
/* Returns an error if the MQTT client could not be stopped; state is then kept. */
esp_err_t airngin_transport_stop(void);
/* Idempotent. Frees the client/queue; returns an error only if they are still live. */
esp_err_t airngin_transport_destroy(void);
/*
 * MANAGED mode provisioning gate. While set, airngin_transport_tick() NEVER starts a new MQTT session, whatever
 * the retry timer, the clock or the (possibly stale) `ip` argument say. It does not stop a running session and
 * does not touch any timer: when cleared, the normal rules (trusted clock, network, back-off, the 60 s
 * restoration delay) apply unchanged. Owner task sets it every iteration from the provisioning FSM state.
 */
void airngin_transport_set_start_gate(bool blocked);

/* MANAGED mode: stop MQTT and wait 60 s after the Wi-Fi IP came back (unchanged since 1.0). */
void airngin_transport_wifi_recovered(int64_t now);

/* EXTERNAL mode helpers (host owns the network). */
/* Do not start a new MQTT session before this time (replaces any earlier hold, including a back-off). */
void airngin_transport_hold_until(int64_t not_before_us);
/* End the current session now; queued TX is discarded and an undelivered READY notice is dropped. */
esp_err_t airngin_transport_drop_session(void);
/*
 * Install new cloud credentials: ends the session, destroys the old client, rebuilds the topics from the new
 * project code and creates a new client (new host, user, password, client id). Queues and the rx/tx counters
 * are kept; the credential-rejection latch is cleared. No connection is attempted before not_before_us.
 * On failure nothing is half-applied that the next call cannot repair.
 */
esp_err_t airngin_transport_replace_credentials(const airngin_profile *profile, int64_t not_before_us);
/* Notifications: same bounded queue and readiness rule as enqueue_publish(). Payload/topic are built by the owner task.
 * ESP_ERR_INVALID_STATE: not ready; ESP_ERR_NO_MEM: queue full. */
esp_err_t airngin_transport_enqueue_notification(bool to_user, int channel, const char *code, size_t code_len);
/* Complete inbound commands dropped because the command queue was full. */
uint32_t airngin_transport_dropped_count(void);

/* MQTT session established (CONNACK) / and the AirNgin subscription acknowledged. */
bool airngin_transport_connected(void);
bool airngin_transport_ready(void);
void airngin_transport_counters(uint32_t *rx_dropped, uint32_t *tx_rejected, uint32_t *tx_failed);

/*
 * Copy key/value into one bounded heap block and queue it for the owner task (never blocks).
 * ESP_ERR_INVALID_STATE: transport not ready (no offline buffering); ESP_ERR_NO_MEM: queue full
 * or allocation failed (counted in tx_rejected). Safe from any task. Lengths must be pre-validated.
 */
esp_err_t airngin_transport_enqueue_publish(const char *key, size_t key_len,
                                            const char *value, size_t value_len);

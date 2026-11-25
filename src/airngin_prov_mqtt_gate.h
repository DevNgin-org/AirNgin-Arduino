#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "airngin_prov_fsm.h"
#include "airngin_transport.h"

/*
 * MANAGED mode: "MQTT must not start or reconnect while provisioning is active, opening, requested or
 * transitioning from STA to the provisioning AP (and until station mode has been restored)".
 *
 * The gate is derived from the provisioning state machine - never from a delay:
 *   panel_open        the SoftAP/HTTP panel is open (or being closed, see expiry_pending)
 *   manual_pending    airngin_request_provisioning() was recorded and the owner has not finished handling it
 *   marking           credential recovery: the recovery panel is about to open
 *   restart_or_saved  a new profile was saved: the device is about to restart
 *   expiry_pending    the panel timed out and is being closed
 *   cancel_pending    the panel is being closed after airngin_cancel_provisioning()
 *   resume_pending    station mode is still being restored after a manual session
 * When none holds, the existing rules decide (trusted clock, IP, back-off, 60 s restoration delay).
 */
static inline bool airngin_prov_blocks_mqtt(const airngin_prov_fsm_t *f, bool panel_open, bool manual_pending,
                                            bool marking, bool restart_or_saved)
{
    return panel_open || manual_pending || marking || restart_or_saved || f->expiry_pending || f->cancel_pending ||
           f->resume_pending;
}

/*
 * The owner calls this INSTEAD of airngin_transport_tick() directly, right after airngin_prov_step(): the gate is
 * evaluated from the state AFTER the step (the panel the step just opened, the request it consumed) and applied
 * before the transport can look at its start condition. `ip` is the snapshot taken at the top of the iteration and
 * may be stale (the STA was stopped by the step itself) - which is exactly why the gate must not depend on it.
 */
static inline int airngin_prov_transport_tick(const airngin_prov_fsm_t *f, const airngin_prov_in_t *in,
                                              bool panel_open_now, bool request_recorded_now,
                                              bool ip, int64_t now)
{
    /* panel_open_now / request_recorded_now are read AFTER the step (the request flag is read live, so a request
     * recorded while this iteration was running is covered too); marking / restart / saved come from `in`. */
    airngin_transport_set_start_gate(airngin_prov_blocks_mqtt(f, panel_open_now, request_recorded_now, in->marking,
                                                              in->restart_pending || in->provisioned_bit));
    return airngin_transport_tick(ip, now);
}

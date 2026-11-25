#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "airngin.h"
#include "esp_err.h"

/*
 * Owner-task provisioning decisions (success report, panel timeout, resume of station mode,
 * manual re-provisioning). Pure logic: every side effect goes through `airngin_prov_ops_t`, so
 * the production code uses the real Wi-Fi/HTTP/MQTT functions and the host tests use fakes.
 * Single-threaded: only the owner task calls airngin_prov_step().
 */
#define AIRNGIN_PROV_RESUME_RETRY_US  5000000LL
#define AIRNGIN_PROV_MANUAL_RETRY_US  1000000LL

typedef struct {
    esp_err_t (*stop_mqtt)(void *ctx);                 /* ends the MQTT session (drops queued TX) */
    esp_err_t (*open_panel)(void *ctx);                /* SoftAP + HTTP; must not touch the stored profile */
    esp_err_t (*close_panel)(void *ctx);               /* HTTP + radio off; idempotent */
    esp_err_t (*resume_sta)(void *ctx, int64_t now_us);/* station mode with the stored profile; idempotent */
    void (*emit)(void *ctx, airngin_event_t event, esp_err_t error);
} airngin_prov_ops_t;

typedef struct {
    bool success_reported;
    bool expiry_pending;       /* the 120 s session timed out; the panel is being closed */
    bool cancel_pending;       /* airngin_cancel_provisioning() was accepted; the panel is being closed */
    bool resume_after_panel;   /* the panel replaced a working station session */
    bool resume_pending;
    int64_t resume_retry_at;
    int64_t manual_retry_at;
} airngin_prov_fsm_t;

typedef struct {
    int64_t now_us;
    bool panel_open;
    bool session_expired;
    bool provisioned_bit;    /* an HTTP save completed */
    bool restart_pending;    /* success grace period running */
    bool marking;            /* credential-recovery panel about to open */
    bool recovery;           /* credentials were rejected */
    bool has_profile;        /* a valid stored profile exists */
    bool manual_request;     /* airngin_request_provisioning() pending */
    bool cancel_request;     /* airngin_cancel_provisioning() pending */
    bool session_claimed;    /* a configuration was committed AND verified (success already happened) */
    bool session_saving;     /* a save is in flight inside the HTTP handler */
} airngin_prov_in_t;

typedef struct {
    bool clear_manual_request;
    bool clear_cancel_request;
} airngin_prov_out_t;

/*
 * A provisioning session ends in exactly one of three ways, with this priority when they race:
 *   SUCCESS (profile committed + verified)  >  MANUAL CANCEL / TIMEOUT
 * Once the new profile is committed nothing here may close or roll back the session: the restart owns it.
 * Timeout and cancel share ONE closing path (close_panel + one reason event + STOPPED + optional STA resume), so the
 * cleanup runs exactly once whichever reason arrives first; the other is coalesced.
 */
static inline void airngin_prov_step(airngin_prov_fsm_t *f, const airngin_prov_in_t *in,
                                     airngin_prov_out_t *out, const airngin_prov_ops_t *ops, void *ctx)
{
    const int64_t now = in->now_us;
    bool panel_open = in->panel_open;
    bool manual_request = in->manual_request;
    const bool success_wins = in->provisioned_bit || in->session_claimed || in->restart_pending;
    out->clear_manual_request = false;
    out->clear_cancel_request = false;

    if (in->provisioned_bit && !f->success_reported) {
        f->success_reported = true;
        ops->emit(ctx, AIRNGIN_EVENT_PROVISIONING_SUCCESS, ESP_OK);
    }

    /* Session timeout is detected first, so a timeout and a cancel seen in the same step resolve as a timeout. */
    if (panel_open && !in->restart_pending && !f->expiry_pending && !f->cancel_pending && in->session_expired) {
        f->expiry_pending = true;
    }

    /* Manual cancel (airngin_cancel_provisioning()). */
    if (in->cancel_request) {
        if (success_wins) {
            out->clear_cancel_request = true;      /* SUCCESS WINS: the committed profile is never rolled back */
        } else if (f->expiry_pending || f->cancel_pending) {
            out->clear_cancel_request = true;      /* already closing: coalesced, no second event */
        } else if (!panel_open) {
            out->clear_cancel_request = true;      /* request still pending (panel not open yet) or nothing to cancel */
            if (manual_request) {                  /* an opening that never happened is simply dropped */
                out->clear_manual_request = true;
                manual_request = false;
            }
        } else if (in->session_saving) {
            /* A save is in flight: decide on the next step - it may be about to succeed. */
        } else {
            out->clear_cancel_request = true;
            f->cancel_pending = true;
        }
    }

    /* One closing path for timeout AND cancel. */
    if ((f->expiry_pending || f->cancel_pending) && ops->close_panel(ctx) == ESP_OK) {
        const bool timed_out = f->expiry_pending;
        f->expiry_pending = false;
        f->cancel_pending = false;
        panel_open = false;
        ops->emit(ctx, timed_out ? AIRNGIN_EVENT_PROVISIONING_TIMEOUT : AIRNGIN_EVENT_PROVISIONING_CANCELLED, ESP_OK);
        if (f->resume_after_panel) {
            f->resume_after_panel = false;
            f->resume_pending = true;
            f->resume_retry_at = now;
        }
        ops->emit(ctx, AIRNGIN_EVENT_PROVISIONING_STOPPED, ESP_OK);
    }

    if (f->resume_pending && !panel_open && now >= f->resume_retry_at) {
        if (ops->resume_sta(ctx, now) == ESP_OK) f->resume_pending = false;
        else f->resume_retry_at = now + AIRNGIN_PROV_RESUME_RETRY_US;
    }

    if (manual_request) {
        if (panel_open || in->restart_pending || in->marking) {
            out->clear_manual_request = true;          /* already satisfied / not applicable: coalesce */
        } else if (!f->expiry_pending && !f->cancel_pending && !f->resume_pending && now >= f->manual_retry_at) {
            if (ops->stop_mqtt(ctx) != ESP_OK) {
                f->manual_retry_at = now + AIRNGIN_PROV_MANUAL_RETRY_US;   /* never switch radios under a live client */
            } else {
                out->clear_manual_request = true;
                const bool resumable = in->has_profile && !in->recovery;
                const esp_err_t err = ops->open_panel(ctx);
                if (err == ESP_OK) {
                    f->expiry_pending = false;
                    f->resume_after_panel = resumable;
                    ops->emit(ctx, AIRNGIN_EVENT_PROVISIONING_STARTED, ESP_OK);
                } else {
                    ops->emit(ctx, AIRNGIN_EVENT_PROVISIONING_FAILED, err);
                    if (resumable) {
                        f->resume_pending = true;
                        f->resume_retry_at = now;
                    }
                }
            }
        }
    }
}

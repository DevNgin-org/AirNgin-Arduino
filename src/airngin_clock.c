#include "airngin_clock.h"
#include "airngin_diag.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <sys/time.h>
#include "AirNginConfig.h"
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
#include "esp_netif_sntp.h"   /* time ACQUISITION: managed mode only */
#endif
#include "esp_timer.h"

// FirmwareTrustedClock policy, adapted to ESP-NETIF's serialized native SNTP lifecycle.
// No timezone, heap-owned callback context, mutex-held SDK call, or timestamp-only trust.
// One configured server per attempt supports native SDKs with a single SNTP server slot;
// unsuccessful attempts rotate through the same authoritative platform fallback servers.
static const char *const airngin_clock_servers[] __attribute__((unused)) = {
    "time.cloudflare.com", "time.google.com", "pool.ntp.org"
};
enum airngin_clock_phase { AIRNGIN_CLOCK_IDLE, AIRNGIN_CLOCK_SYNCING, AIRNGIN_CLOCK_READY, AIRNGIN_CLOCK_RETRY };
static enum airngin_clock_phase airngin_clock_phase_value = AIRNGIN_CLOCK_IDLE;
static atomic_bool airngin_clock_signal = ATOMIC_VAR_INIT(false);
static atomic_bool airngin_clock_ready = ATOMIC_VAR_INIT(false);
static bool airngin_clock_active __attribute__((unused));
// EXTERNAL mode: the host owns time acquisition; this module only detects whether the clock is usable.
static bool airngin_clock_external;
static unsigned airngin_clock_server_index __attribute__((unused));
static int64_t airngin_clock_deadline_ms, airngin_clock_last_sync_ms, airngin_clock_last_epoch;
static int64_t airngin_clock_backoff_ms = 15000;

static void __attribute__((unused)) airngin_clock_sync_notice(struct timeval *value)
{
    // Static storage survives every attempt. Only the owner reads the resulting clock/state.
    if (value && value->tv_sec >= 1704067200LL)
        atomic_store_explicit(&airngin_clock_signal, true, memory_order_release);
}

#if AG_DIAG
/* "2026-11-09 14:03:12 UTC" of the current system time (diagnostics only). */
static void airngin_clock_time_text(char *out, size_t cap)
{
    const time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL || strftime(out, cap, "%Y-%m-%d %H:%M:%S UTC", &utc) == 0) {
        snprintf(out, cap, "epoch=%lld", (long long)now);
    }
}
#endif

static void airngin_clock_end_attempt(void)
{
#if CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
    if (airngin_clock_active) {
        // ESP-NETIF removes the notification and synchronizes SNTP stop on the TCP/IP thread.
        // Clear the signal AFTER this barrier so a prior callback cannot seed the next attempt.
        esp_netif_sntp_deinit();
        airngin_clock_active = false;
    }
#endif
    atomic_store_explicit(&airngin_clock_signal, false, memory_order_release);
}

static void __attribute__((unused)) airngin_clock_retry_later(int64_t now_ms)
{
    airngin_clock_end_attempt();
    atomic_store_explicit(&airngin_clock_ready, false, memory_order_release);
    airngin_clock_phase_value = AIRNGIN_CLOCK_RETRY;
    AG_DIAG_W("AG_TIME", "SNTP attempt failed (no sync within 20 s, or SNTP init failed); next attempt in %lld ms",
              (long long)airngin_clock_backoff_ms);
    airngin_clock_deadline_ms = now_ms + airngin_clock_backoff_ms;
    airngin_clock_backoff_ms = airngin_clock_backoff_ms >= 150000 ? 300000 : airngin_clock_backoff_ms * 2;
}

bool airngin_clock_trusted(void)
{
    return atomic_load_explicit(&airngin_clock_ready, memory_order_acquire);
}

void airngin_clock_stop(void)
{
    AG_DIAG_DECL(const bool was_active = airngin_clock_phase_value != AIRNGIN_CLOCK_IDLE)
    atomic_store_explicit(&airngin_clock_ready, false, memory_order_release);
    airngin_clock_end_attempt();
    airngin_clock_phase_value = AIRNGIN_CLOCK_IDLE;
    airngin_clock_backoff_ms = 15000;
    airngin_clock_last_sync_ms = 0;
    airngin_clock_last_epoch = 0;
    AG_DIAG_DO(if (was_active) ESP_LOGI("AG_TIME", "clock module stopped (trust reset)"));
}

void airngin_clock_set_external(bool trust_detection_only)
{
    airngin_clock_external = trust_detection_only;
    atomic_store_explicit(&airngin_clock_ready, false, memory_order_release);
}

void airngin_clock_tick(bool wifi_has_ip)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int64_t epoch = (int64_t)time(NULL);
    if (airngin_clock_external) {
        // Trust DETECTION only: the host set the time (SNTP, cellular, RTC, ...). Same sanity floor as
        // managed mode; no SNTP is started or stopped here.
        const bool trusted_now = epoch >= 1704067200LL;
#if AG_DIAG
        static int ext_prev = -1;   /* -1 = nothing logged yet; log only changes */
        if (ext_prev != (int)trusted_now) {
            char text[40];
            airngin_clock_time_text(text, sizeof(text));
            if (trusted_now) AG_DIAG_I("AG_TIME", "clock trusted: system time=%s (set by the host)", text);
            else AG_DIAG_W("AG_TIME", "clock not trusted (system time=%s, before 2024-01-01); MQTT postponed", text);
            ext_prev = (int)trusted_now;
        }
#endif
        atomic_store_explicit(&airngin_clock_ready, trusted_now, memory_order_release);
        return;
    }
#if !CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI
    (void)now_ms; (void)wifi_has_ip;
    return;
#else
    if (airngin_clock_phase_value == AIRNGIN_CLOCK_READY) {
        if (epoch < 1704067200LL || epoch + 5 < airngin_clock_last_epoch ||
            now_ms - airngin_clock_last_sync_ms >= 86400000LL) {
            AG_DIAG_W("AG_TIME", "clock trust lost: %s", epoch < 1704067200LL ? "system time went before 2024-01-01"
                      : epoch + 5 < airngin_clock_last_epoch ? "system time jumped backwards" : "24 h re-synchronization due");
            airngin_clock_stop();
            // The next tick must start a fresh native attempt; trust stays false until its callback.
            return;
        }
        if (atomic_exchange_explicit(&airngin_clock_signal, false, memory_order_acq_rel))
            airngin_clock_last_sync_ms = now_ms;
        airngin_clock_last_epoch = epoch;
        return;
    }
    if (!wifi_has_ip) {
        if (airngin_clock_phase_value == AIRNGIN_CLOCK_SYNCING) {
            AG_DIAG_W("AG_TIME", "network lost during SNTP synchronization: SNTP stopped");
            airngin_clock_stop();
        }
        return;
    }
    if (airngin_clock_phase_value == AIRNGIN_CLOCK_SYNCING) {
        if (atomic_exchange_explicit(&airngin_clock_signal, false, memory_order_acq_rel)) {
            const int64_t synchronized_epoch = (int64_t)time(NULL);
            if (synchronized_epoch >= 1704067200LL) {
                airngin_clock_last_sync_ms = now_ms;
                airngin_clock_last_epoch = synchronized_epoch;
                airngin_clock_backoff_ms = 15000;
                airngin_clock_phase_value = AIRNGIN_CLOCK_READY;
                atomic_store_explicit(&airngin_clock_ready, true, memory_order_release);
#if AG_DIAG
                {
                    char text[40];
                    airngin_clock_time_text(text, sizeof(text));
                    AG_DIAG_I("AG_TIME", "system time=%s", text);
                    AG_DIAG_I("AG_TIME", "clock trusted");
                }
#endif
                return;
            }
        }
        if (now_ms >= airngin_clock_deadline_ms) airngin_clock_retry_later(now_ms);
        return;
    }
    if (airngin_clock_phase_value == AIRNGIN_CLOCK_RETRY && now_ms < airngin_clock_deadline_ms) return;
    airngin_clock_end_attempt();
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(airngin_clock_servers[airngin_clock_server_index]);
    airngin_clock_server_index = (airngin_clock_server_index + 1) % 3;
    config.wait_for_sync = false;
    config.sync_cb = airngin_clock_sync_notice;
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        airngin_clock_retry_later(now_ms);
        return;
    }
    airngin_clock_active = true;
    airngin_clock_phase_value = AIRNGIN_CLOCK_SYNCING;
    airngin_clock_deadline_ms = now_ms + 20000;
    AG_DIAG_I("AG_TIME", "SNTP started (server %s)", airngin_clock_servers[(airngin_clock_server_index + 2) % 3]);
    AG_DIAG_I("AG_TIME", "waiting for trusted clock (up to 20 s per attempt)");
#endif
}

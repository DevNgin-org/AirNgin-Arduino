#pragma once

/*
 * AirNgin diagnostic logging.
 *
 *   CONFIG_AIRNGIN_DIAGNOSTIC_LOGS        master switch; everything below compiles out when off
 *   CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE     topics / payload text / queue + heap details
 *   CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS DANGEROUS: plain-text credentials (hardware debugging only)
 *
 * SECRET POLICY (strict): Wi-Fi and MQTT passwords are never printed, not even in verbose mode. Allowed:
 * length, a PASS/FAIL, and a short diagnostic fingerprint (first 4 bytes of SHA-256, 8 hex characters; never
 * stored, never used for authentication). Only LOG_SECRETS=y changes that, and it then prints a boot warning.
 * The provisioning SoftAP password is never printed, even with LOG_SECRETS.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "AirNginConfig.h"
#include "esp_err.h"

#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_LOGS
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOGS 0
#endif
#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE
#define CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE 0
#endif
#ifndef CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS
#define CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS 0
#endif

/* esp_err_t as "ESP_ERR_INVALID_STATE (0x103)" - usable in normal (non-diagnostic) logs too. */
#define AG_ERRF "%s (0x%x)"
#define AG_ERRV(e) esp_err_to_name(e), (unsigned)(e)

#if CONFIG_AIRNGIN_DIAGNOSTIC_LOGS

#include "esp_log.h"
#include "airngin.h"

#define AG_DIAG 1
#define AG_DIAG_SECRETS (CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS)
#define AG_DIAG_VERBOSE_ON (CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE)

#define AG_DIAG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define AG_DIAG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define AG_DIAG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#if CONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE
#define AG_DIAG_V(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#else
#define AG_DIAG_V(tag, fmt, ...) ((void)0)
#endif

/* Statements that exist only in diagnostic builds (keeps call sites free of #if). */
#define AG_DIAG_DO(stmt) do { stmt; } while (0)
/* A declaration that exists only in diagnostic builds (no trailing semicolon at the call site). */
#define AG_DIAG_DECL(decl) decl;

/* Readable names. */
const char *airngin_diag_state_name(airngin_state_t state);
const char *airngin_diag_event_name(airngin_event_t event);
const char *airngin_diag_mode_name(airngin_network_mode_t mode);
static inline const char *airngin_diag_yesno(bool v) { return v ? "YES" : "NO"; }

/* 8 hex characters of SHA-256(s[0..n)) - comparison aid only. out must hold 9 bytes. */
void airngin_diag_fingerprint(const char *s, size_t n, char out[9]);

/*
 * One credential-like field:  name = <hidden> len=N fp=XXXXXXXX      (default)
 *                             name = "escaped text" len=N             (LOG_SECRETS only)
 * `is_secret` fields (passwords) obey the policy; non-secret fields are always shown.
 */
void airngin_diag_field(const char *tag, const char *name, const char *value, size_t len, bool is_secret);
/* VERBOSE + LOG_SECRETS: bounded hex dump of a field. No-op otherwise. */
void airngin_diag_hex(const char *tag, const char *name, const char *value, size_t len);
/* Raw received bytes in escaped, chunked form. LOG_SECRETS only; otherwise just "<N bytes, hidden>". */
void airngin_diag_dump_raw(const char *tag, const char *label, const char *data, size_t len);
/* Complete explicit-length buffer, escaped and emitted in bounded chunks. Never allocates. */
void airngin_diag_print_buffer(const char *tag, const uint8_t *data, size_t len);
/* Prints the "SECRET DIAGNOSTICS ENABLED" warning when applicable. */
void airngin_diag_secret_warning(void);
/* Heap (and, when called from the owner task, stack) snapshot. VERBOSE only. */
void airngin_diag_snapshot(const char *where, bool in_owner_task);
/* Owner-task call: logs changes of network / clock / mqtt / ready / provisioning / rejection (state changes only). */
void airngin_diag_status_watch(void);
void airngin_diag_status_watch_reset(void);
/* Human readable status box from the PUBLIC status API (also usable by applications). */
void airngin_diag_print_status(const char *tag, const char *title);

#else  /* diagnostics compiled out */

#define AG_DIAG 0
#define AG_DIAG_SECRETS 0
#define AG_DIAG_VERBOSE_ON 0
#define AG_DIAG_I(tag, fmt, ...) ((void)0)
#define AG_DIAG_W(tag, fmt, ...) ((void)0)
#define AG_DIAG_E(tag, fmt, ...) ((void)0)
#define AG_DIAG_V(tag, fmt, ...) ((void)0)
#define AG_DIAG_DO(stmt) ((void)0)
#define AG_DIAG_DECL(decl)
#define airngin_diag_status_watch() ((void)0)
#define airngin_diag_status_watch_reset() ((void)0)
#define airngin_diag_snapshot(where, owner) ((void)0)
#define airngin_diag_print_status(tag, title) ((void)0)
#define airngin_diag_secret_warning() ((void)0)
#define airngin_diag_field(tag, name, value, len, is_secret) ((void)0)
#define airngin_diag_hex(tag, name, value, len) ((void)0)
#define airngin_diag_dump_raw(tag, label, data, len) ((void)0)
#define airngin_diag_print_buffer(tag, data, len) ((void)0)

#endif

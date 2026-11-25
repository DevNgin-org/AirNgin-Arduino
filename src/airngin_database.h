#pragma once
/*
 * AirNgin Cloud Database - internal interface.
 *
 * Contract (docs tutorial-database + backend DeviceCloudController, validators and handlers; NOT MQTT):
 *   host app.airngin.com:443 (HTTPS), base /api/v1.0/DeviceCloud
 *   SET    POST   /AddNewData      form body  Username Password DeviceSerial ProjectCode Key Value   (create OR update)
 *   GET    POST   /GetDeviceCloud  query      Username Password DeviceSerial ProjectCode Key         (empty body)
 *   DELETE DELETE /DeleteData      query      Username Password DeviceSerial ProjectCode Key
 *   Username/Password = the device's MQTT credentials; DeviceSerial exactly 14, ProjectCode exactly 16 characters.
 *   Key 1..20 [A-Za-z0-9]; Value non-empty string (opaque, may be JSON).
 *   Reply: JSON {"isSuccess":bool,"message":text,"returnType":..,"requestId":..,"data":{"value":text}} ("data" only for GET).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "airngin.h"
#include "esp_err.h"
#include "AirNginConfig.h"

#if CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE
#define AIRNGIN_DB_ENABLED 1
#else
#define AIRNGIN_DB_ENABLED 0
#endif

#ifndef CONFIG_AIRNGIN_DATABASE_VALUE_MAX
#define CONFIG_AIRNGIN_DATABASE_VALUE_MAX 4096
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_TIMEOUT_MS
#define CONFIG_AIRNGIN_DATABASE_TIMEOUT_MS 15000
#endif
#ifndef CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
#define CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS 0
#endif

#define AIRNGIN_DATABASE_VALUE_MAX   ((size_t)CONFIG_AIRNGIN_DATABASE_VALUE_MAX)
/* A stored value may come back JSON-escaped (up to 6 bytes per input byte) inside the reply envelope. */
#define AIRNGIN_DATABASE_RESPONSE_MAX (6U * AIRNGIN_DATABASE_VALUE_MAX + 1024U)
#define AIRNGIN_DATABASE_MESSAGE_MAX 160U
#define AIRNGIN_DATABASE_HOST        "app.airngin.com"
#define AIRNGIN_DATABASE_PROJECT_LEN 16U     /* server rule */
#define AIRNGIN_DATABASE_SERIAL_LEN  14U     /* server rule */

typedef struct {
    const char *method;          /* "POST" or "DELETE" */
    const char *path;            /* absolute path on AIRNGIN_DATABASE_HOST */
    char *query;                 /* GET/DELETE credentials+key; contains the password: never log */
    char *body;                  /* SET form body; contains the password: never log */
    size_t body_len;
    const char *content_type;    /* NULL for DELETE */
} airngin_db_http_request_t;

/* Platform services. Production: airngin_database_http.c (esp_http_client + worker task). Host tests supply fakes. */
typedef struct {
    /* Run fn(arg) on a separate task/context. Must not run it inline in the caller. */
    esp_err_t (*submit)(void (*fn)(void *), void *arg);
    /* One blocking HTTPS exchange. *response is heap memory (NUL terminated) owned by the caller, at most response_cap bytes.
     * Return ESP_ERR_TIMEOUT when timeout_ms elapsed in total, ESP_ERR_NO_MEM when the reply exceeds response_cap. */
    esp_err_t (*perform)(const airngin_db_http_request_t *req, uint32_t timeout_ms, size_t response_cap,
                         char **response, size_t *response_len, int *http_status);
    /* Ask the in-flight perform() to return as soon as possible. */
    void (*cancel)(void);
} airngin_db_ops_t;
const airngin_db_ops_t *airngin_database_platform_ops(void);

/* ---- pure helpers (host tested) ---- */
esp_err_t airngin_db_validate_key(const char *key, size_t *length);
esp_err_t airngin_db_validate_value(const char *value, size_t *length);
/* RFC 3986 percent-encoding (unreserved A-Z a-z 0-9 - . _ ~ kept). Returns the output length or -1 when `cap` (incl. NUL) is too small. */
int airngin_db_urlencode(const char *in, size_t n, char *out, size_t cap);
typedef struct { const char *user; const char *pass; const char *serial; const char *project; } airngin_db_credentials_t;
/* Builds the HTTP request for one operation. The caller frees query/body with airngin_db_request_free(). */
esp_err_t airngin_db_build_request(airngin_database_operation_t op, const airngin_db_credentials_t *creds, const char *key,
                                   const char *value, airngin_db_http_request_t *out);
void airngin_db_request_free(airngin_db_http_request_t *req);
typedef struct {
    bool server_success;
    char *value;                 /* GET success: heap copy, NUL terminated; else NULL */
    size_t value_len;
    char message[AIRNGIN_DATABASE_MESSAGE_MAX + 1U];
} airngin_db_reply_t;
/* ESP_OK parsed (inspect server_success), ESP_ERR_INVALID_RESPONSE otherwise. The caller frees reply->value. */
esp_err_t airngin_db_parse_reply(airngin_database_operation_t op, int http_status, const char *body, size_t len, airngin_db_reply_t *reply);

#if AIRNGIN_DB_ENABLED
/* Successful-init warning for the explicitly dangerous temporary request logger. */
void airngin_database_request_log_warning(void);
/* Owner task: deliver a finished request as AIRNGIN_EVENT_DATABASE_RESULT. */
void airngin_database_poll(void);
/* Cancel an in-flight request, wait (bounded) for it, drop undelivered results, wipe credentials. ESP_ERR_TIMEOUT: retry later. */
esp_err_t airngin_database_shutdown(void);
/* simple counters for diagnostics */
void airngin_database_counters(uint32_t *requests, uint32_t *failures, uint32_t *timeouts);
#else
static inline void airngin_database_request_log_warning(void) {}
static inline void airngin_database_poll(void) {}
static inline esp_err_t airngin_database_shutdown(void) { return ESP_OK; }
#endif

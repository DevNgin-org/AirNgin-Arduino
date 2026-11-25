#include "airngin_database.h"
#include "airngin_internal.h"
#include "airngin_clock.h"
#include "airngin_diag.h"
#include "airngin_utf8.h"
#include "airngin_topic.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !AIRNGIN_DB_ENABLED
/* Feature compiled out: the public API exists and says so. */
esp_err_t airngin_database_set(const char *key, const char *value, uint32_t *request_id) { (void)key; (void)value; (void)request_id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t airngin_database_get(const char *key, uint32_t *request_id) { (void)key; (void)request_id; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t airngin_database_delete(const char *key, uint32_t *request_id) { (void)key; (void)request_id; return ESP_ERR_NOT_SUPPORTED; }
#else

#define DB_BASE_PATH "/api/v1.0/DeviceCloud/"
#define DB_FORM "application/x-www-form-urlencoded"
#define DB_WAIT_SHUTDOWN_STEPS 500            /* x 10 ms */

void airngin_database_request_log_warning(void)
{
#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
    printf("\n################################################################\n");
    printf("WARNING: CLOUD DATABASE REQUEST PARAMETER LOGGING ENABLED\n");
    printf("HTTP USERNAME/PASSWORD WILL BE PRINTED IN PLAIN TEXT\n");
    printf("TEST BUILD ONLY - DO NOT USE IN PRODUCTION\n");
    printf("################################################################\n\n");
    fflush(stdout);
#endif
}

#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
/*
 * DANGEROUS TEMPORARY HARDWARE DEBUGGING. These helpers deliberately disclose credentials and the complete
 * outgoing request. They are compiled out unless CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS=y, independently
 * of the normal AirNgin diagnostic/secret settings. Remove or disable after the hardware investigation.
 */
static const char *db_debug_method(airngin_database_operation_t op)
{
    return op == AIRNGIN_DATABASE_OP_DELETE ? "DELETE" : "POST";
}

static const char *db_debug_path(airngin_database_operation_t op)
{
    return op == AIRNGIN_DATABASE_OP_SET ? DB_BASE_PATH "AddNewData" :
           op == AIRNGIN_DATABASE_OP_GET ? DB_BASE_PATH "GetDeviceCloud" : DB_BASE_PATH "DeleteData";
}

static const char *db_debug_title(airngin_database_operation_t op)
{
    return op == AIRNGIN_DATABASE_OP_SET ? "ADD NEW DATA" :
           op == AIRNGIN_DATABASE_OP_GET ? "GET DEVICE CLOUD" : "DELETE DATA";
}

static bool db_debug_exact(const char *a, const char *b)
{
    const size_t a_len = strlen(a);
    const size_t b_len = strlen(b);
    return a_len == b_len && memcmp(a, b, a_len) == 0;
}

static void db_debug_hex(const char *label, const char *value, size_t len)
{
    printf("%s HEX:\n", label);
    for (size_t i = 0; i < len; ++i) {
        printf("%02X%s", (unsigned)(unsigned char)value[i], i + 1U == len ? "" : " ");
    }
    printf("\n");
}

static void db_debug_mismatch(const char *side, const char *field, const char *value)
{
    const size_t len = strlen(value);
    printf("\n%s %s:\n", side, field);
    printf("value = \"%s\"\n", value);
    printf("len   = %u\n", (unsigned)len);
    printf("hex   = ");
    for (size_t i = 0; i < len; ++i) {
        printf("%02X%s", (unsigned)(unsigned char)value[i], i + 1U == len ? "" : " ");
    }
    printf("\n");
}

static void db_debug_compare(const char *label, const char *mqtt, const char *database)
{
    const bool match = db_debug_exact(mqtt, database);
    printf("%-21s: %s\n", label, match ? "YES" : "NO");
    if (!match) {
        db_debug_mismatch("MQTT", label, mqtt);
        db_debug_mismatch("DB", label, database);
    }
}

static void db_debug_log_encoded_request(const airngin_db_http_request_t *req)
{
    printf("\n==================================================\n");
    if (req->body) {
        const size_t body_strlen = strlen(req->body);
        printf("AG_DB_REQUEST: FINAL FORM BODY\n");
        printf("==================================================\n\n");
        printf("%s\n\n", req->body);
        printf("Body length             : %u\n", (unsigned)req->body_len);
        printf("Content-Length expected : %u\n", (unsigned)req->body_len);
        printf("Body buffer length      : %u\n", (unsigned)body_strlen);
        printf("strlen(body)             : %u\n", (unsigned)body_strlen);
        if (body_strlen != req->body_len) {
            printf("AG_DB_REQUEST ERROR: CONTENT LENGTH MISMATCH\n");
        }
    } else {
        printf("AG_DB_REQUEST: FINAL QUERY\n");
        printf("==================================================\n\n");
        printf("Method : %s\n", req->method);
        printf("URL    : https://%s%s\n", AIRNGIN_DATABASE_HOST, req->path);
        printf("\nFINAL QUERY:\n%s\n", req->query ? req->query : "");
        printf("Query length: %u\n", (unsigned)(req->query ? strlen(req->query) : 0U));
    }
    printf("\n==================================================\n");
    fflush(stdout);
}

static void db_debug_log_http_response(esp_err_t transport_status, int http_status,
                                       const char *response, size_t response_len)
{
    printf("\n==================================================\n");
    printf("AG_DB_HTTP: RAW SERVER RESPONSE\n");
    printf("==================================================\n\n");
    printf("Transport status         : %s (0x%x)\n", esp_err_to_name(transport_status), (unsigned)transport_status);
    printf("HTTP status              : %d\n", http_status);
    printf("Raw response body length : %u\n", (unsigned)response_len);
    printf("Complete raw response body:\n");
    if (response && response_len) {
        /* fwrite emits the exact received buffer and is not constrained by ESP_LOG's per-line formatting size. */
        (void)fwrite(response, 1U, response_len, stdout);
        printf("\n");
    } else {
        printf("<empty>\n");
    }
    printf("==================================================\n");
    fflush(stdout);
}

static void db_debug_log_parsed_response(esp_err_t parsed, const airngin_db_reply_t *reply)
{
    printf("\nAG_DB_HTTP: PARSED RESPONSE\n");
    printf("Parse status   : %s (0x%x)\n", esp_err_to_name(parsed), (unsigned)parsed);
    if (parsed == ESP_OK && reply) {
        printf("server_success : %s\n", reply->server_success ? "true" : "false");
        printf("message        : \"%s\"\n", reply->message);
        printf("value length   : %u\n", (unsigned)reply->value_len);
        if (reply->value) {
            printf("value          : ");
            (void)fwrite(reply->value, 1U, reply->value_len, stdout);
            printf("\n");
        }
    }
    printf("==================================================\n");
    fflush(stdout);
}
#endif

esp_err_t airngin_db_validate_key(const char *key, size_t *length)
{
    if (!key) return ESP_ERR_INVALID_ARG;
    const size_t n = strnlen(key, AIRNGIN_DATABASE_KEY_MAX + 1U);
    if (n == 0U || n > AIRNGIN_DATABASE_KEY_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < n; ++i) {
        if (!isalnum((unsigned char)key[i]) || (unsigned char)key[i] > 127U) return ESP_ERR_INVALID_ARG;   /* English letters and digits only */
    }
    if (length) *length = n;
    return ESP_OK;
}

esp_err_t airngin_db_validate_value(const char *value, size_t *length)
{
    if (!value) return ESP_ERR_INVALID_ARG;
    const size_t n = strnlen(value, AIRNGIN_DATABASE_VALUE_MAX + 1U);
    if (n == 0U || n > AIRNGIN_DATABASE_VALUE_MAX) return ESP_ERR_INVALID_SIZE;       /* the server rejects an empty value */
    if (!airngin_utf8_valid((const unsigned char *)value, n)) return ESP_ERR_INVALID_ARG;
    if (length) *length = n;
    return ESP_OK;
}

int airngin_db_urlencode(const char *in, size_t n, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!in || !out || !cap) return -1;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)in[i];
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        if (o + (keep ? 1U : 3U) + 1U > cap) return -1;
        if (keep) out[o++] = (char)c;
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15U]; }
    }
    out[o] = '\0';
    return (int)o;
}

static size_t encoded_len(const char *s)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        n += ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~') ? 1U : 3U;
    }
    return n;
}

/* Appends "name=" + percent-encoded value (and a leading '&' unless first). Returns false when it does not fit. */
static bool append_field(char *buf, size_t cap, size_t *used, const char *name, const char *value, bool first)
{
    const size_t need = (first ? 0U : 1U) + strlen(name) + 1U + encoded_len(value);
    if (*used + need + 1U > cap) return false;
    if (!first) buf[(*used)++] = '&';
    memcpy(buf + *used, name, strlen(name)); *used += strlen(name);
    buf[(*used)++] = '=';
    const int n = airngin_db_urlencode(value, strlen(value), buf + *used, cap - *used);
    if (n < 0) return false;
    *used += (size_t)n;
    return true;
}

esp_err_t airngin_db_build_request(airngin_database_operation_t op, const airngin_db_credentials_t *c, const char *key, const char *value,
                                   airngin_db_http_request_t *out)
{
    if (!c || !c->user || !c->pass || !c->serial || !c->project || !key || !out) return ESP_ERR_INVALID_ARG;
    if (op == AIRNGIN_DATABASE_OP_SET && !value) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    size_t need = encoded_len(c->user) + encoded_len(c->pass) + encoded_len(c->serial) + encoded_len(c->project) + encoded_len(key) + 128U;
    if (op == AIRNGIN_DATABASE_OP_SET) need += encoded_len(value);
    char *buf = malloc(need);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t used = 0;
    bool ok = append_field(buf, need, &used, "Username", c->user, true) && append_field(buf, need, &used, "Password", c->pass, false) &&
              append_field(buf, need, &used, "DeviceSerial", c->serial, false) && append_field(buf, need, &used, "ProjectCode", c->project, false) &&
              append_field(buf, need, &used, "Key", key, false);
    if (ok && op == AIRNGIN_DATABASE_OP_SET) ok = append_field(buf, need, &used, "Value", value, false);
    if (!ok) { airngin_secure_zero(buf, need); free(buf); return ESP_ERR_INVALID_SIZE; }
    switch (op) {
    case AIRNGIN_DATABASE_OP_SET:
        out->method = "POST"; out->path = DB_BASE_PATH "AddNewData"; out->body = buf; out->body_len = used; out->content_type = DB_FORM;
        break;
    case AIRNGIN_DATABASE_OP_GET:      /* the server binds GetDeviceCloud from the QUERY STRING; the body stays empty */
        out->method = "POST"; out->path = DB_BASE_PATH "GetDeviceCloud"; out->query = buf; out->content_type = DB_FORM;
        break;
    case AIRNGIN_DATABASE_OP_DELETE:
        out->method = "DELETE"; out->path = DB_BASE_PATH "DeleteData"; out->query = buf;
        break;
    default:
        airngin_secure_zero(buf, need); free(buf); memset(out, 0, sizeof(*out));
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void airngin_db_request_free(airngin_db_http_request_t *req)
{
    if (!req) return;
    if (req->query) { airngin_secure_zero(req->query, strlen(req->query)); free(req->query); }
    if (req->body) { airngin_secure_zero(req->body, req->body_len); free(req->body); }
    memset(req, 0, sizeof(*req));
}

esp_err_t airngin_db_parse_reply(airngin_database_operation_t op, int http_status, const char *body, size_t len, airngin_db_reply_t *reply)
{
    if (!reply) return ESP_ERR_INVALID_ARG;
    memset(reply, 0, sizeof(*reply));
    if (http_status != 200 || !body || len == 0U || len > AIRNGIN_DATABASE_RESPONSE_MAX) return ESP_ERR_INVALID_RESPONSE;
    if (memchr(body, 0, len)) return ESP_ERR_INVALID_RESPONSE;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, len, &end, false);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = ESP_ERR_INVALID_RESPONSE;
    const cJSON *success = cJSON_GetObjectItem(root, "isSuccess");     /* case-insensitive on purpose */
    if (!cJSON_IsObject(root) || !cJSON_IsBool(success)) goto done;
    reply->server_success = cJSON_IsTrue(success);
    const cJSON *message = cJSON_GetObjectItem(root, "message");
    if (cJSON_IsString(message)) {
        size_t n = strnlen(message->valuestring, AIRNGIN_DATABASE_MESSAGE_MAX);
        while (n > 0U && n < strlen(message->valuestring) && ((unsigned char)message->valuestring[n] & 0xC0U) == 0x80U) --n;   /* do not cut a UTF-8 character */
        memcpy(reply->message, message->valuestring, n);
        reply->message[n] = '\0';
    }
    if (op == AIRNGIN_DATABASE_OP_GET && reply->server_success) {
        const cJSON *data = cJSON_GetObjectItem(root, "data");
        const cJSON *value = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "value") : NULL;
        if (!cJSON_IsString(value)) goto done;                       /* success without data.value: unusable */
        const size_t n = strlen(value->valuestring);
        reply->value = malloc(n + 1U);
        if (!reply->value) { err = ESP_ERR_NO_MEM; goto done; }
        memcpy(reply->value, value->valuestring, n + 1U);
        reply->value_len = n;
    }
    err = ESP_OK;
done:
    cJSON_Delete(root);
    return err;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Request lifecycle: ONE request at a time. accepted -> (worker) running -> finished -> delivered by poll()      */
/* ------------------------------------------------------------------------------------------------------------ */
typedef struct {
    airngin_database_operation_t op;
    uint32_t id;
    char key[AIRNGIN_DATABASE_KEY_MAX + 1U];
    char *value;
    size_t value_len;
    /* Per-request ownership is required because the HTTPS worker is asynchronous. This is a snapshot of the exact
     * effective MQTT identity, never an independently configured Database credential store. */
    airngin_effective_cloud_credentials_t credentials;
} db_job;

#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
static void db_debug_log_logical_request(const db_job *job,
                                         const airngin_effective_cloud_credentials_t *mqtt)
{
    const size_t username_len = strlen(job->credentials.username);
    const size_t password_len = strlen(job->credentials.password);
    const size_t serial_len = strlen(job->credentials.device_serial);
    const size_t project_len = strlen(job->credentials.project_code);
    const size_t key_len = strlen(job->key);
    const size_t value_len = job->value ? strlen(job->value) : 0U;

    printf("\n==================================================\n");
    printf("AG_DB_REQUEST: %s\n", db_debug_title(job->op));
    printf("==================================================\n\n");
    printf("Method      : %s\n", db_debug_method(job->op));
    printf("URL         : https://%s%s\n", AIRNGIN_DATABASE_HOST, db_debug_path(job->op));
    if (job->op != AIRNGIN_DATABASE_OP_DELETE) printf("Content-Type: %s\n", DB_FORM);
    printf("\nRAW PARAMETERS:\n\n");
    printf("Username     : \"%s\"\n", job->credentials.username);
    printf("Username len : %u\n", (unsigned)username_len);
    printf("Username strlen = %u\n", (unsigned)strlen(job->credentials.username));
    db_debug_hex("Username", job->credentials.username, username_len);
    printf("\nPassword     : \"%s\"\n", job->credentials.password);
    printf("Password len : %u\n", (unsigned)password_len);
    printf("Password strlen = %u\n", (unsigned)strlen(job->credentials.password));
    db_debug_hex("Password", job->credentials.password, password_len);
    printf("\nDeviceSerial : \"%s\"\n", job->credentials.device_serial);
    printf("Serial len   : %u\n", (unsigned)serial_len);
    printf("\nProjectCode  : \"%s\"\n", job->credentials.project_code);
    printf("Project len  : %u\n", (unsigned)project_len);
    printf("\nKey          : \"%s\"\n", job->key);
    printf("Key len      : %u\n", (unsigned)key_len);
    if (job->op == AIRNGIN_DATABASE_OP_SET) {
        printf("\nValue        : \"%s\"\n", job->value);
        printf("Value len    : %u\n", (unsigned)value_len);
    }
    printf("\nMQTT EFFECTIVE vs DATABASE REQUEST\n");
    printf("----------------------------------\n\n");
    db_debug_compare("Username exact match", mqtt->username, job->credentials.username);
    db_debug_compare("Password exact match", mqtt->password, job->credentials.password);
    db_debug_compare("Project exact match", mqtt->project_code, job->credentials.project_code);
    printf("\n==================================================\n");
    fflush(stdout);
}
#endif

typedef struct {
    airngin_database_operation_t op;
    uint32_t id;
    char key[AIRNGIN_DATABASE_KEY_MAX + 1U];
    esp_err_t status;
    int http_status;
    bool server_success;
    char *value;
    size_t value_len;
    char message[AIRNGIN_DATABASE_MESSAGE_MAX + 1U];
} db_done;

static portMUX_TYPE db_lock = portMUX_INITIALIZER_UNLOCKED;
static db_job *db_job_slot;       /* accepted, waiting for / inside the worker (the "busy" marker) */
static bool db_running;           /* the worker is executing */
static bool db_cancel;            /* shutdown requested */
static db_done *db_done_slot;     /* finished, waiting for poll() */
__attribute__((unused)) static uint32_t db_next_id;
static uint32_t db_requests, db_failures, db_timeouts;

void airngin_database_counters(uint32_t *r, uint32_t *f, uint32_t *t)
{
    portENTER_CRITICAL(&db_lock);
    if (r) *r = db_requests;
    if (f) *f = db_failures;
    if (t) *t = db_timeouts;
    portEXIT_CRITICAL(&db_lock);
}

__attribute__((unused)) static const char *op_name(airngin_database_operation_t op)
{
    return op == AIRNGIN_DATABASE_OP_SET ? "SET" : op == AIRNGIN_DATABASE_OP_GET ? "GET" : op == AIRNGIN_DATABASE_OP_DELETE ? "DELETE" : "?";
}

static void job_free(db_job *job)
{
    if (!job) return;
    if (job->value) { airngin_secure_zero(job->value, job->value_len); free(job->value); }
    airngin_secure_zero(&job->credentials, sizeof(job->credentials));
    free(job);
}
static void done_free(db_done *d)
{
    if (!d) return;
    if (d->value) { airngin_secure_zero(d->value, d->value_len); free(d->value); }
    free(d);
}

static bool credentials_equal(const airngin_effective_cloud_credentials_t *a,
                              const airngin_effective_cloud_credentials_t *b)
{
    return a->source == b->source &&
           strcmp(a->project_code, b->project_code) == 0 &&
           strcmp(a->username, b->username) == 0 &&
           strcmp(a->password, b->password) == 0 &&
           strcmp(a->client_id, b->client_id) == 0 &&
           strcmp(a->broker_host, b->broker_host) == 0 &&
           strcmp(a->device_serial, b->device_serial) == 0;
}

__attribute__((unused)) static const char *credential_source_name(airngin_effective_credentials_source_t source)
{
    return source == AIRNGIN_EFFECTIVE_CREDENTIALS_EXTERNAL_RUNTIME
               ? "EXTERNAL RUNTIME CREDENTIALS" : "MANAGED NVS PROFILE";
}

/* Worker context: one blocking HTTPS exchange, then post the result. Never retries. */
__attribute__((unused)) static void db_execute(void *arg)
{
    db_job *job = arg;
    const airngin_db_ops_t *ops = airngin_database_platform_ops();
    db_done *done = calloc(1, sizeof(*done));
    airngin_db_credentials_t creds = {0};
    bool cancelled;

    portENTER_CRITICAL(&db_lock);
    db_running = true;
    cancelled = db_cancel;
    portEXIT_CRITICAL(&db_lock);

    if (done) {
        done->op = job->op; done->id = job->id;
        memcpy(done->key, job->key, sizeof(done->key));
        done->status = ESP_ERR_INVALID_STATE;
        if (!cancelled && ops) {
            /* Refuse a queued request if credential rotation made its snapshot stale before HTTPS began. The comparison
             * is byte-for-byte; fingerprints below are diagnostics only. */
            airngin_effective_cloud_credentials_t current = {0};
            const esp_err_t current_err = airngin_runtime_get_effective_cloud_credentials(&current);
            const bool current_match = current_err == ESP_OK && credentials_equal(&job->credentials, &current);
#if AG_DIAG
            char db_user_fp[9], db_pass_fp[9], mqtt_user_fp[9], mqtt_pass_fp[9];
            const size_t user_len = strlen(job->credentials.username);
            const size_t pass_len = strlen(job->credentials.password);
            airngin_diag_fingerprint(job->credentials.username, user_len, db_user_fp);
            airngin_diag_fingerprint(job->credentials.password, pass_len, db_pass_fp);
            if (current_err == ESP_OK) {
                airngin_diag_fingerprint(current.username, strlen(current.username), mqtt_user_fp);
                airngin_diag_fingerprint(current.password, strlen(current.password), mqtt_pass_fp);
            } else {
                strcpy(mqtt_user_fp, "????????");
                strcpy(mqtt_pass_fp, "????????");
            }
            AG_DIAG_I("AG_DB_AUTH", "credential source = %s", credential_source_name(job->credentials.source));
            AG_DIAG_I("AG_DB_AUTH", "username length      = %u", (unsigned)user_len);
            AG_DIAG_I("AG_DB_AUTH", "username fingerprint = %s", db_user_fp);
            AG_DIAG_I("AG_DB_AUTH", "password length      = %u", (unsigned)pass_len);
            AG_DIAG_I("AG_DB_AUTH", "password fingerprint = %s", db_pass_fp);
            AG_DIAG_I("AG_DB_AUTH", "project code         = %s", job->credentials.project_code);
            AG_DIAG_I("AG_DB_AUTH", "device serial        = %s", job->credentials.device_serial);
            AG_DIAG_I("AG_DB_AUTH", "MQTT username fingerprint = %s", mqtt_user_fp);
            AG_DIAG_I("AG_DB_AUTH", "MQTT password fingerprint = %s", mqtt_pass_fp);
            AG_DIAG_I("AG_DB_AUTH", "MQTT/HTTPS username match = %s",
                      current_err == ESP_OK && strcmp(job->credentials.username, current.username) == 0 ? "YES" : "NO");
            AG_DIAG_I("AG_DB_AUTH", "MQTT/HTTPS password match = %s",
                      current_err == ESP_OK && strcmp(job->credentials.password, current.password) == 0 ? "YES" : "NO");
#endif
            if (!current_match) {
                airngin_secure_zero(&current, sizeof(current));
                done->status = ESP_ERR_INVALID_STATE;
                goto worker_done;
            }

#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
            /* Deliberately before airngin_db_build_request(): show the unmodified logical bytes that will be encoded. */
            db_debug_log_logical_request(job, &current);
#endif
            airngin_secure_zero(&current, sizeof(current));

            creds = (airngin_db_credentials_t){ job->credentials.username, job->credentials.password,
                                                job->credentials.device_serial, job->credentials.project_code };
            airngin_db_http_request_t req = {0};
            esp_err_t err = airngin_db_build_request(job->op, &creds, job->key, job->value, &req);
            if (err == ESP_OK) {
                char *response = NULL;
                size_t response_len = 0;
                int http_status = 0;
#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
                /* req.body/query is the actual buffer handed to the production HTTP client below. */
                db_debug_log_encoded_request(&req);
#endif
                AG_DIAG_I("AG_DB", "HTTP %s %s%s", req.method, AIRNGIN_DATABASE_HOST, req.path);          /* never the query / body: they carry the password */
                err = ops->perform(&req, (uint32_t)CONFIG_AIRNGIN_DATABASE_TIMEOUT_MS, AIRNGIN_DATABASE_RESPONSE_MAX, &response, &response_len, &http_status);
#if AG_DIAG
                AG_DIAG_I("AG_DB_RX", "==================================================");
                AG_DIAG_I("AG_DB_RX", "HTTPS RESPONSE");
                AG_DIAG_I("AG_DB_RX", "==================================================");
                AG_DIAG_I("AG_DB_RX", "Operation   : %s", op_name(job->op));
                AG_DIAG_I("AG_DB_RX", "Request ID  : %u", (unsigned)job->id);
                AG_DIAG_I("AG_DB_RX", "Transport   : %s", esp_err_to_name(err));
                AG_DIAG_I("AG_DB_RX", "HTTP status : %d", http_status);
                AG_DIAG_I("AG_DB_RX", "Body len    : %u", (unsigned)response_len);
                AG_DIAG_I("AG_DB_RX", "Raw body:");
                airngin_diag_print_buffer("AG_DB_RX", (const uint8_t *)response, response_len);
#endif
#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
                db_debug_log_http_response(err, http_status, response, response_len);
#endif
                done->http_status = http_status;
                portENTER_CRITICAL(&db_lock);
                cancelled = db_cancel;
                portEXIT_CRITICAL(&db_lock);
                if (cancelled) {
                    done->status = ESP_ERR_INVALID_STATE;                      /* the SDK is stopping */
                } else if (err != ESP_OK) {
                    done->status = err;
#if AG_DIAG
                    AG_DIAG_I("AG_DB_RX", "==================================================");
#endif
                } else {
                    airngin_db_reply_t reply = {0};
                    const esp_err_t parsed = airngin_db_parse_reply(job->op, http_status, response, response_len, &reply);
#if AG_DIAG
                    AG_DIAG_I("AG_DB_RX", "Parsed:");
                    AG_DIAG_I("AG_DB_RX", "Parse result   : %s", esp_err_to_name(parsed));
                    if (parsed == ESP_OK) {
                        AG_DIAG_I("AG_DB_RX", "Server success: %s", reply.server_success ? "YES" : "NO");
                        AG_DIAG_I("AG_DB_RX", "Key           : %s", job->key);
                        AG_DIAG_I("AG_DB_RX", "Value len     : %u", (unsigned)reply.value_len);
                        AG_DIAG_I("AG_DB_RX", "Value:");
                        airngin_diag_print_buffer("AG_DB_RX", (const uint8_t *)reply.value, reply.value_len);
                        AG_DIAG_I("AG_DB_RX", "Message       : %s", reply.message);
                    }
                    AG_DIAG_I("AG_DB_RX", "==================================================");
#endif
#if CONFIG_AIRNGIN_DATABASE_LOG_REQUEST_PARAMS
                    db_debug_log_parsed_response(parsed, &reply);
#endif
                    if (parsed != ESP_OK) {
                        done->status = parsed;
                    } else {
                        done->server_success = reply.server_success;
                        done->status = reply.server_success ? ESP_OK : ESP_FAIL;
                        done->value = reply.value;
                        done->value_len = reply.value_len;
                        memcpy(done->message, reply.message, sizeof(done->message));
                    }
                }
                if (response) { airngin_secure_zero(response, response_len); free(response); }
            } else {
                done->status = err;
            }
            airngin_db_request_free(&req);
        }
    }
worker_done:
    airngin_secure_zero(&creds, sizeof(creds));

    portENTER_CRITICAL(&db_lock);
    if (done) {
        if (done->status != ESP_OK) db_failures++;
        if (done->status == ESP_ERR_TIMEOUT) db_timeouts++;
    }
    db_done_slot = done;                  /* NULL (out of memory): the request is simply dropped, the slot is freed below */
    db_job_slot = NULL;
    db_running = false;
    portEXIT_CRITICAL(&db_lock);
    job_free(job);
}

static esp_err_t db_request(airngin_database_operation_t op, const char *key, const char *value, uint32_t *request_id)
{
#if CONFIG_AIRNGIN_USE_AES && !CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD
    (void)op; (void)key; (void)value; (void)request_id;
    return ESP_ERR_NOT_SUPPORTED;         /* no application-layer AES form exists; never silently TLS-only in an AES build */
#else
    size_t key_len = 0, value_len = 0;
    esp_err_t err = airngin_db_validate_key(key, &key_len);
    if (err == ESP_OK && op == AIRNGIN_DATABASE_OP_SET) err = airngin_db_validate_value(value, &value_len);
    if (err != ESP_OK) return err;
    if (airngin_get_state() != AIRNGIN_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    const airngin_db_ops_t *ops = airngin_database_platform_ops();
    if (!ops || !ops->submit) return ESP_ERR_NOT_SUPPORTED;

    airngin_runtime_status_t st;
    airngin_runtime_get_status(&st);
    airngin_effective_cloud_credentials_t effective = {0};
    const esp_err_t credentials_err = airngin_runtime_get_effective_cloud_credentials(&effective);
    const size_t username_len = strnlen(effective.username, sizeof(effective.username));
    const size_t password_len = strnlen(effective.password, sizeof(effective.password));
    const size_t project_len = strnlen(effective.project_code, sizeof(effective.project_code));
    const size_t serial_len = strnlen(effective.device_serial, sizeof(effective.device_serial));
    const bool creds_ok = credentials_err == ESP_OK &&
                          username_len > 0U && username_len < sizeof(effective.username) &&
                          password_len > 0U && password_len < sizeof(effective.password) &&
                          project_len == AIRNGIN_DATABASE_PROJECT_LEN &&
                          serial_len == AIRNGIN_DATABASE_SERIAL_LEN;
    if (!creds_ok || !st.network_connected || !airngin_clock_trusted()) {
        AG_DIAG_W("AG_DB", "request refused: %s", !creds_ok ? "no credentials, or project code / serial length is not 16 / 14 (server rule)" :
                  !st.network_connected ? "network is down" : "clock not trusted (TLS)");
        airngin_secure_zero(&effective, sizeof(effective));
        return ESP_ERR_INVALID_STATE;
    }

    db_job *job = calloc(1, sizeof(*job));
    if (!job) {
        airngin_secure_zero(&effective, sizeof(effective));
        return ESP_ERR_NO_MEM;
    }
    job->credentials = effective;
    airngin_secure_zero(&effective, sizeof(effective));
    memcpy(job->key, key, key_len);
    if (op == AIRNGIN_DATABASE_OP_SET) {
        job->value = malloc(value_len + 1U);
        if (!job->value) { free(job); return ESP_ERR_NO_MEM; }
        memcpy(job->value, value, value_len + 1U);
        job->value_len = value_len;
    }
    job->op = op;
    portENTER_CRITICAL(&db_lock);
    const bool busy = db_job_slot != NULL || db_done_slot != NULL || db_cancel;
    if (!busy) {
        job->id = ++db_next_id;
        db_job_slot = job;
        db_requests++;
    }
    portEXIT_CRITICAL(&db_lock);
    if (busy) {
        job_free(job);
        AG_DIAG_W("AG_DB", "request refused: another request is still outstanding (one at a time)");
        return ESP_ERR_NO_MEM;
    }
    AG_DIAG_I("AG_DB", "request operation=%s key=\"%s\" payload_len=%u", op_name(op), key, (unsigned)value_len);
    AG_DIAG_I("AG_DB", "queued request_id=%u", (unsigned)job->id);
    const uint32_t id = job->id;
    err = ops->submit(db_execute, job);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&db_lock);
        db_job_slot = NULL;
        db_requests--;
        portEXIT_CRITICAL(&db_lock);
        job_free(job);
        return err == ESP_ERR_NO_MEM ? err : ESP_ERR_NO_MEM;
    }
    if (request_id) *request_id = id;
    return ESP_OK;
#endif
}

esp_err_t airngin_database_set(const char *key, const char *value, uint32_t *request_id) { return db_request(AIRNGIN_DATABASE_OP_SET, key, value, request_id); }
esp_err_t airngin_database_get(const char *key, uint32_t *request_id) { return db_request(AIRNGIN_DATABASE_OP_GET, key, NULL, request_id); }
esp_err_t airngin_database_delete(const char *key, uint32_t *request_id) { return db_request(AIRNGIN_DATABASE_OP_DELETE, key, NULL, request_id); }

void airngin_database_poll(void)
{
    portENTER_CRITICAL(&db_lock);
    db_done *done = db_done_slot;
    db_done_slot = NULL;
    portEXIT_CRITICAL(&db_lock);
    if (!done) return;
    AG_DIAG_I("AG_DB", "RX response request_id=%u", (unsigned)done->id);
    AG_DIAG_I("AG_DB", "operation=%s result=%s http_status=%d value_len=%u", op_name(done->op),
              done->status == ESP_OK ? "SUCCESS" : done->status == ESP_FAIL ? "SERVER_REFUSED" : esp_err_to_name(done->status), done->http_status,
              (unsigned)done->value_len);
#if AG_DIAG_SECRETS
    if (done->value) AG_DIAG_V("AG_DB", "value=\"%.96s\"", done->value);
#endif
    airngin_event_data_t event = { .type = AIRNGIN_EVENT_DATABASE_RESULT, .error = done->status };
    event.data.database = (airngin_database_event_t){
        .operation = done->op, .request_id = done->id, .status = done->status, .http_status = done->http_status,
        .server_success = done->server_success, .key = done->key, .value = done->value, .value_len = done->value_len, .message = done->message,
    };
    airngin_internal_emit(&event);
    done_free(done);
}

esp_err_t airngin_database_shutdown(void)
{
    const airngin_db_ops_t *ops = airngin_database_platform_ops();
    portENTER_CRITICAL(&db_lock);
    db_cancel = true;
    const bool inflight = db_job_slot != NULL;
    portEXIT_CRITICAL(&db_lock);
    if (inflight && ops && ops->cancel) ops->cancel();
    for (int i = 0; i < DB_WAIT_SHUTDOWN_STEPS; ++i) {
        portENTER_CRITICAL(&db_lock);
        const bool still = db_job_slot != NULL;
        portEXIT_CRITICAL(&db_lock);
        if (!still) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&db_lock);
    if (db_job_slot != NULL) { portEXIT_CRITICAL(&db_lock); return ESP_ERR_TIMEOUT; }   /* retryable: stop() calls again */
    db_done *done = db_done_slot;
    db_done_slot = NULL;
    db_cancel = false;
    portEXIT_CRITICAL(&db_lock);
    done_free(done);
    return ESP_OK;
}
#endif /* AIRNGIN_DB_ENABLED */

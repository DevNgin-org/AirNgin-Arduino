#include "airngin_protocol.h"
#include "airngin_internal.h"
#include "airngin_utf8.h"
#include "airngin_diag.h"
#include "airngin.h"
#include "cJSON.h"
#include <ctype.h>
#include <string.h>

#if AG_DIAG
static const char *ag_proto_why;   /* diagnostic: which check rejected the last parse */
#endif

static cJSON *ag_parse_json(const char *text, size_t length)
{
    if (!text || !length || length > 4096U || memchr(text, 0, length)) {
        AG_DIAG_DO(ag_proto_why = !length ? "empty" : length > 4096U ? "longer than 4096 bytes" : "embedded NUL byte");
        return NULL;
    }
    if (!airngin_utf8_valid((const unsigned char *)text, length)) {
        AG_DIAG_DO(ag_proto_why = "invalid UTF-8");
        return NULL;
    }
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\\') {
            if (i + 5U < length && text[i + 1U] == 'u' &&
                !memcmp(text + i + 2U, "0000", 4U)) {
                AG_DIAG_DO(ag_proto_why = "escaped NUL (\\u0000)");
                return NULL;
            }
            ++i;
        }
    }
    const char *end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts(text, length, &end, false);
    if (!json) {
        AG_DIAG_DO(ag_proto_why = "JSON syntax error");
        return NULL;
    }
    while (end < text + length && isspace((unsigned char)*end)) ++end;
    if (end != text + length) {
        AG_DIAG_DO(ag_proto_why = "trailing bytes after the JSON value");
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

static bool ag_extract_exact_pair(cJSON *object, cJSON **key, cJSON **value)
{
    bool valid = cJSON_IsObject(object);
    *key = NULL;
    *value = NULL;
    if (!valid) return false;
    for (cJSON *item = object->child; item; item = item->next) {
        if (!strcmp(item->string, "key") && !*key) *key = item;
        else if (!strcmp(item->string, "value") && !*value) *value = item;
        else valid = false;
    }
    return valid && cJSON_IsString(*key) && cJSON_IsString(*value);
}

esp_err_t airngin_protocol_build_report(const char *key, const char *value,
                                        char *out, size_t capacity, size_t *out_len)
{
    if (!key || !value || !out || !out_len || capacity < 2U) return ESP_ERR_INVALID_ARG;
    *out_len = 0;
    out[0] = '\0';
    cJSON *array = cJSON_CreateArray();
    cJSON *object = cJSON_CreateObject();
    if (!array || !object ||
        !cJSON_AddStringToObject(object, "key", key) ||
        !cJSON_AddStringToObject(object, "value", value)) {
        cJSON_Delete(object);
        cJSON_Delete(array);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(array, object);
    if (!cJSON_PrintPreallocated(array, out, (int)capacity, false)) {
        cJSON_Delete(array);
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    cJSON_Delete(array);
    *out_len = strlen(out);
    return ESP_OK;
}

esp_err_t airngin_protocol_process(const char *payload,
                                   size_t payload_len,
                                   char *response,
                                   size_t response_capacity,
                                   size_t *response_len)
{
    if (!payload || !response || !response_len || response_capacity < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    *response_len = 0;
    response[0] = '\0';

    cJSON *outer = ag_parse_json(payload, payload_len);
    if (!cJSON_IsObject(outer)) {
        AG_DIAG_W("AG_PROTO", "payload UTF-8 / JSON parse ... FAIL: %s", outer ? "top-level value is not an object" : ag_proto_why);
        cJSON_Delete(outer);
        return ESP_ERR_INVALID_ARG;
    }
    AG_DIAG_I("AG_PROTO", "payload UTF-8 ........ PASS");
    AG_DIAG_I("AG_PROTO", "JSON parse ........... PASS");

    cJSON *serial = NULL;
    cJSON *data = NULL;
    bool valid = true;
    for (cJSON *item = outer->child; item; item = item->next) {
        if (!strcmp(item->string, "deviceSerial") && !serial) serial = item;
        else if (!strcmp(item->string, "data") && !data) data = item;
        else valid = false;
    }
    if (!valid || !cJSON_IsString(serial) || !cJSON_IsString(data) ||
        strcmp(serial->valuestring, airngin_internal_serial())) {
        AG_DIAG_W("AG_PROTO", "serial/topic ......... FAIL: %s", !valid ? "unexpected or duplicate envelope field"
                  : (!cJSON_IsString(serial) || !cJSON_IsString(data)) ? "deviceSerial/data missing or not strings"
                  : "deviceSerial does not match this device");
        cJSON_Delete(outer);
        return ESP_ERR_INVALID_ARG;
    }
    AG_DIAG_I("AG_PROTO", "serial/topic ......... PASS");

    cJSON *inner = ag_parse_json(data->valuestring, strlen(data->valuestring));
    cJSON *key = NULL;
    cJSON *value = NULL;
    if (!ag_extract_exact_pair(inner, &key, &value)) {
        AG_DIAG_W("AG_PROTO", "command data ......... FAIL: %s", inner ? "must be exactly {\"key\":string,\"value\":string}" : ag_proto_why);
        cJSON_Delete(inner);
        cJSON_Delete(outer);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t key_len = strlen(key->valuestring);
    const size_t value_len = strlen(value->valuestring);
    if (!key_len || key_len > AIRNGIN_COMMAND_KEY_MAX ||
        value_len > AIRNGIN_COMMAND_VALUE_MAX) {
        AG_DIAG_W("AG_PROTO", "command size ......... FAIL: key %u bytes (1..%u), value %u bytes (max %u)", (unsigned)key_len,
                  (unsigned)AIRNGIN_COMMAND_KEY_MAX, (unsigned)value_len, (unsigned)AIRNGIN_COMMAND_VALUE_MAX);
        cJSON_Delete(inner);
        cJSON_Delete(outer);
        return ESP_ERR_INVALID_SIZE;
    }
    AG_DIAG_I("AG_CMD", "==================================================");
    AG_DIAG_I("AG_CMD", "COMMAND RECEIVED");
    AG_DIAG_I("AG_CMD", "==================================================");
#if CONFIG_AIRNGIN_USE_AES && !AG_DIAG_SECRETS
    AG_DIAG_I("AG_CMD", "Key   : <hidden, %u bytes>", (unsigned)key_len);
    AG_DIAG_I("AG_CMD", "Value : <hidden, %u bytes>", (unsigned)value_len);
#else
    AG_DIAG_I("AG_CMD", "Key (%u bytes):", (unsigned)key_len);
    airngin_diag_print_buffer("AG_CMD", (const uint8_t *)key->valuestring, key_len);
    AG_DIAG_I("AG_CMD", "Value (%u bytes):", (unsigned)value_len);
    airngin_diag_print_buffer("AG_CMD", (const uint8_t *)value->valuestring, value_len);
#endif
    AG_DIAG_I("AG_CMD", "==================================================");

    airngin_command_t command = {0};
    memcpy(command.key, key->valuestring, key_len + 1U);
    memcpy(command.value, value->valuestring, value_len + 1U);
    airngin_command_result_t result = {0};
    const esp_err_t dispatch_err = airngin_internal_dispatch(&command, &result);
    cJSON_Delete(inner);
    cJSON_Delete(outer);
    if (dispatch_err != ESP_OK) return dispatch_err;
    if (!result.has_value) return ESP_OK;

    const size_t result_len = strnlen(result.value, AIRNGIN_COMMAND_VALUE_MAX + 1U);
    if (result_len > AIRNGIN_COMMAND_VALUE_MAX) return ESP_ERR_INVALID_SIZE;

    const char *response_key = command.key;
    if (result.has_key) {
        const size_t result_key_len = strnlen(result.key, AIRNGIN_COMMAND_KEY_MAX + 1U);
        if (!result_key_len || result_key_len > AIRNGIN_COMMAND_KEY_MAX) return ESP_ERR_INVALID_SIZE;
        response_key = result.key;
    }

    return airngin_protocol_build_report(response_key, result.value, response, response_capacity, response_len);
}

#include "airngin_notify.h"
#include "airngin_utf8.h"
#include "cJSON.h"
#include "AirNginConfig.h"
#include <string.h>

esp_err_t airngin_notify_validate_code(const char *code, size_t *length)
{
    if (!code) return ESP_ERR_INVALID_ARG;
    const size_t n = strnlen(code, AIRNGIN_NOTIFICATION_CODE_MAX + 1U);
    if (n == 0U || n > AIRNGIN_NOTIFICATION_CODE_MAX) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char)code[i];
        if (c < 0x20U || c == 0x7FU) return ESP_ERR_INVALID_ARG;
    }
    if (!airngin_utf8_valid((const unsigned char *)code, n)) return ESP_ERR_INVALID_ARG;
    if (length) *length = n;
    return ESP_OK;
}

bool airngin_notify_channel_valid(int channel)
{
    return channel == AIRNGIN_NOTIFICATION_SMS || channel == AIRNGIN_NOTIFICATION_PUSH;
}

esp_err_t airngin_notify_build(bool to_user, const char *code, int channel, char *out, size_t capacity, size_t *length)
{
    if (!out || !length || capacity < 2U) return ESP_ERR_INVALID_ARG;
    *length = 0;
    out[0] = '\0';
    const esp_err_t v = airngin_notify_validate_code(code, NULL);
    if (v != ESP_OK) return v;
    if (to_user && !airngin_notify_channel_valid(channel)) return ESP_ERR_INVALID_ARG;

    cJSON *object = cJSON_CreateObject();
    if (!object || !cJSON_AddStringToObject(object, "code", code)) {
        cJSON_Delete(object);
        return ESP_ERR_NO_MEM;
    }
    if (to_user) {
#if CONFIG_AIRNGIN_NOTIFICATION_TYPE_AS_STRING
        if (!cJSON_AddStringToObject(object, "type", channel == AIRNGIN_NOTIFICATION_SMS ? "20" : "30")) {
#else
        if (!cJSON_AddNumberToObject(object, "type", (double)channel)) {
#endif
            cJSON_Delete(object);
            return ESP_ERR_NO_MEM;
        }
    }
    const bool ok = cJSON_PrintPreallocated(object, out, (int)capacity, false);
    cJSON_Delete(object);
    if (!ok) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    *length = strlen(out);
    return ESP_OK;
}

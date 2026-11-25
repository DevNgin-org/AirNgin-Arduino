#pragma once
/* AirNgin notification payloads (host tested). Topics come from airngin_topic.h. */
#include <stdbool.h>
#include <stddef.h>
#include "airngin.h"
#include "esp_err.h"

/* Validate a message code: 1..AIRNGIN_NOTIFICATION_CODE_MAX bytes, valid UTF-8, no control characters.
 * ESP_ERR_INVALID_ARG: NULL / bad UTF-8 / control character; ESP_ERR_INVALID_SIZE: empty or too long. */
esp_err_t airngin_notify_validate_code(const char *code, size_t *length);
bool airngin_notify_channel_valid(int channel);

/* Center: {"code":"..."}. User: {"code":"...","type":20|30} (type is a JSON number unless
 * CONFIG_AIRNGIN_NOTIFICATION_TYPE_AS_STRING). The code is escaped by the JSON serializer. */
esp_err_t airngin_notify_build(bool to_user, const char *code, int channel, char *out, size_t capacity, size_t *length);

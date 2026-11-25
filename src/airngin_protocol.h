#pragma once

#include <stddef.h>
#include "esp_err.h"

#define AIRNGIN_PROTOCOL_RESPONSE_MAX 4096U

/*
 * Build the canonical device->server payload `[{"key":..,"value":..}]` (unformatted JSON).
 * Used for command responses and for public publish/report so both share one serializer.
 * Returns ESP_ERR_INVALID_SIZE if `capacity` cannot hold the result, ESP_ERR_NO_MEM on allocation failure.
 */
esp_err_t airngin_protocol_build_report(const char *key, const char *value,
                                        char *out, size_t capacity, size_t *out_len);

/*
 * Parse one complete AirNgin ServerToDevice payload, dispatch it to the host
 * application, and build the DeviceToServer payload. A zero response length
 * means the message was valid but intentionally produced no response.
 */
esp_err_t airngin_protocol_process(const char *payload,
                                   size_t payload_len,
                                   char *response,
                                   size_t response_capacity,
                                   size_t *response_len);

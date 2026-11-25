#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AIRNGIN_TEST_CONSOLE_PARSE_MAX 11000U

typedef enum {
    ATC_CMD_NONE = 0,
    ATC_CMD_HELP, ATC_CMD_STATUS,
    ATC_CMD_PUBLISH, ATC_CMD_REPORT,
    ATC_CMD_DB_SET, ATC_CMD_DB_GET, ATC_CMD_DB_DELETE,
    ATC_CMD_SMS_CENTER, ATC_CMD_SMS_USER, ATC_CMD_PUSH_USER,
    ATC_CMD_TIME_UTC, ATC_CMD_TIME_TEHRAN, ATC_CMD_TIME_SHOW, ATC_CMD_TIME_OFF,
    ATC_CMD_PROV_STATUS, ATC_CMD_PROV_START, ATC_CMD_PROV_CANCEL,
    ATC_CMD_MQTT_TLS_STATUS, ATC_CMD_MQTT_TLS_ON, ATC_CMD_MQTT_TLS_OFF,
} airngin_test_command_kind_t;

typedef enum {
    ATC_PARSE_OK = 0,
    ATC_PARSE_EMPTY,
    ATC_PARSE_UNKNOWN,
    ATC_PARSE_USAGE,
    ATC_PARSE_OVERSIZED,
} airngin_test_parse_result_t;

typedef struct {
    airngin_test_command_kind_t kind;
    char *arg1;
    char *arg2;
    char *unknown;
    const char *usage;
} airngin_test_command_t;

/** Parse in place. Returned argument pointers remain inside `line`. */
airngin_test_parse_result_t airngin_test_console_parse(char *line, airngin_test_command_t *out);

bool airngin_test_console_command_available(airngin_test_command_kind_t kind,
                                            bool database_enabled,
                                            bool insecure_mqtt_test_enabled);

#ifdef __cplusplus
}
#endif

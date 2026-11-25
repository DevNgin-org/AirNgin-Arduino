#include "AirNginTestConsoleParser.h"

#include <stdbool.h>
#include <string.h>

static char *token(char **cursor)
{
    while (**cursor == ' ') ++*cursor;
    if (**cursor == '\0') return NULL;
    char *start = *cursor;
    while (**cursor != '\0' && **cursor != ' ') ++*cursor;
    if (**cursor != '\0') *(*cursor)++ = '\0';
    return start;
}

static char *line_remainder(char **cursor)
{
    while (**cursor == ' ') ++*cursor;
    if (**cursor == '\0') return NULL;
    char *start = *cursor;
    const size_t n = strlen(start);
    if (n >= 2U && start[0] == '"' && start[n - 1U] == '"') {
        start[n - 1U] = '\0';
        ++start;
    }
    return start;
}

static airngin_test_parse_result_t no_args(char **cursor, airngin_test_command_t *out,
                                           airngin_test_command_kind_t kind, const char *usage)
{
    out->kind = kind;
    out->usage = usage;
    return token(cursor) == NULL ? ATC_PARSE_OK : ATC_PARSE_USAGE;
}

static airngin_test_parse_result_t one_arg(char **cursor, airngin_test_command_t *out,
                                           airngin_test_command_kind_t kind, const char *usage)
{
    out->kind = kind;
    out->usage = usage;
    out->arg1 = token(cursor);
    return out->arg1 != NULL && token(cursor) == NULL ? ATC_PARSE_OK : ATC_PARSE_USAGE;
}

static airngin_test_parse_result_t key_value(char **cursor, airngin_test_command_t *out,
                                             airngin_test_command_kind_t kind, const char *usage)
{
    out->kind = kind;
    out->usage = usage;
    out->arg1 = token(cursor);
    out->arg2 = line_remainder(cursor);
    return out->arg1 != NULL && out->arg2 != NULL ? ATC_PARSE_OK : ATC_PARSE_USAGE;
}

airngin_test_parse_result_t airngin_test_console_parse(char *line, airngin_test_command_t *out)
{
    if (line == NULL || out == NULL) return ATC_PARSE_USAGE;
    memset(out, 0, sizeof(*out));
    if (strnlen(line, AIRNGIN_TEST_CONSOLE_PARSE_MAX + 1U) > AIRNGIN_TEST_CONSOLE_PARSE_MAX) {
        return ATC_PARSE_OVERSIZED;
    }

    char *cursor = line;
    char *first = token(&cursor);
    if (first == NULL) return ATC_PARSE_EMPTY;
    if (!strcmp(first, "help")) return no_args(&cursor, out, ATC_CMD_HELP, "help");
    if (!strcmp(first, "status")) return no_args(&cursor, out, ATC_CMD_STATUS, "status");
    if (!strcmp(first, "publish")) return key_value(&cursor, out, ATC_CMD_PUBLISH, "publish <key> <value>");
    if (!strcmp(first, "report")) return key_value(&cursor, out, ATC_CMD_REPORT, "report <key> <value>");

    if (!strcmp(first, "db")) {
        char *sub = token(&cursor);
        if (sub && !strcmp(sub, "set")) return key_value(&cursor, out, ATC_CMD_DB_SET, "db set <key> <value>");
        if (sub && !strcmp(sub, "get")) return one_arg(&cursor, out, ATC_CMD_DB_GET, "db get <key>");
        if (sub && !strcmp(sub, "delete")) return one_arg(&cursor, out, ATC_CMD_DB_DELETE, "db delete <key>");
        out->usage = "db set <key> <value> | db get <key> | db delete <key>";
        return ATC_PARSE_USAGE;
    }
    if (!strcmp(first, "sms")) {
        char *target = token(&cursor);
        if (target && !strcmp(target, "center")) return one_arg(&cursor, out, ATC_CMD_SMS_CENTER, "sms center <code>");
        if (target && !strcmp(target, "user")) return one_arg(&cursor, out, ATC_CMD_SMS_USER, "sms user <code>");
        out->usage = "sms center <code> | sms user <code>";
        return ATC_PARSE_USAGE;
    }
    if (!strcmp(first, "push")) {
        char *target = token(&cursor);
        if (target && !strcmp(target, "user")) return one_arg(&cursor, out, ATC_CMD_PUSH_USER, "push user <code>");
        out->usage = "push user <code>";
        return ATC_PARSE_USAGE;
    }
    if (!strcmp(first, "time")) {
        char *sub = token(&cursor);
        if (sub && !strcmp(sub, "utc")) return no_args(&cursor, out, ATC_CMD_TIME_UTC, "time utc");
        if (sub && !strcmp(sub, "tehran")) return no_args(&cursor, out, ATC_CMD_TIME_TEHRAN, "time tehran");
        if (sub && !strcmp(sub, "show")) return no_args(&cursor, out, ATC_CMD_TIME_SHOW, "time show");
        if (sub && !strcmp(sub, "off")) return no_args(&cursor, out, ATC_CMD_TIME_OFF, "time off");
        out->usage = "time utc | time tehran | time show | time off";
        return ATC_PARSE_USAGE;
    }
    if (!strcmp(first, "prov")) {
        char *sub = token(&cursor);
        if (sub && !strcmp(sub, "status")) return no_args(&cursor, out, ATC_CMD_PROV_STATUS, "prov status");
        if (sub && !strcmp(sub, "start")) return no_args(&cursor, out, ATC_CMD_PROV_START, "prov start");
        if (sub && !strcmp(sub, "cancel")) return no_args(&cursor, out, ATC_CMD_PROV_CANCEL, "prov cancel");
        out->usage = "prov status | prov start | prov cancel";
        return ATC_PARSE_USAGE;
    }
    if (!strcmp(first, "mqtt")) {
        char *tls = token(&cursor);
        char *sub = token(&cursor);
        if (!tls || strcmp(tls, "tls") || !sub) {
            out->usage = "mqtt tls status";
            return ATC_PARSE_USAGE;
        }
        if (!strcmp(sub, "status")) return no_args(&cursor, out, ATC_CMD_MQTT_TLS_STATUS, "mqtt tls status");
        if (!strcmp(sub, "on")) return no_args(&cursor, out, ATC_CMD_MQTT_TLS_ON, "mqtt tls on");
        if (!strcmp(sub, "off")) return no_args(&cursor, out, ATC_CMD_MQTT_TLS_OFF, "mqtt tls off");
        out->usage = "mqtt tls status";
        return ATC_PARSE_USAGE;
    }

    out->unknown = first;
    return ATC_PARSE_UNKNOWN;
}

bool airngin_test_console_command_available(airngin_test_command_kind_t kind,
                                            bool database_enabled,
                                            bool insecure_mqtt_test_enabled)
{
    if (kind == ATC_CMD_DB_SET || kind == ATC_CMD_DB_GET || kind == ATC_CMD_DB_DELETE) {
        return database_enabled;
    }
    if (kind == ATC_CMD_MQTT_TLS_ON || kind == ATC_CMD_MQTT_TLS_OFF) {
        return insecure_mqtt_test_enabled;
    }
    return true;
}

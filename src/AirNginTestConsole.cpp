#include "AirNginTestConsole.h"
#include "AirNginTestConsoleParser.h"
#include "airngin_version.h"
#include "AirNginConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdlib.h>
#include <string.h>

#define ATC_EVENT_DEPTH 8U
#define ATC_LINE_SIZE (CONFIG_AIRNGIN_DATABASE_VALUE_MAX + 256U)
#define ATC_DB_MESSAGE_MAX 160U

typedef struct {
    airngin_event_t type;
    esp_err_t error;
    union {
        struct { int reason; uint32_t discarded; } mqtt;
        airngin_time_info_t time;
        struct {
            airngin_database_operation_t operation;
            uint32_t request_id;
            esp_err_t status;
            int http_status;
            bool server_success;
            size_t value_len;
            char key[AIRNGIN_DATABASE_KEY_MAX + 1U];
            char message[ATC_DB_MESSAGE_MAX + 1U];
        } database;
    } data;
    char value[1];
} atc_event_t;

AirNginTestConsoleClass AirNginConsole;

static void atc_copy(char *out, size_t cap, const char *s) {
    if (!cap) return;
    if (!s) { out[0] = 0; return; }
    size_t n = strnlen(s, cap - 1U); memcpy(out, s, n); out[n] = 0;
}
static const char *life(airngin_state_t s) {
    static const char *n[] = {"UNINITIALIZED","INITIALIZED","STARTING","RUNNING","STOPPING"};
    return (unsigned)s < 5U ? n[s] : "UNKNOWN";
}

esp_err_t AirNginTestConsoleClass::begin(Stream &io) {
    if (_io) return ESP_ERR_INVALID_STATE;
    _line = (char *)malloc(ATC_LINE_SIZE);
    if (!_line) return ESP_ERR_NO_MEM;
    _eventQueue = xQueueCreate(ATC_EVENT_DEPTH, sizeof(atc_event_t *));
    if (!_eventQueue) { free(_line); _line = nullptr; return ESP_ERR_NO_MEM; }
    _io = &io;
    help();
    _io->println();
    _io->println("WARNING: SMS/push operations may consume AirNgin wallet credit.");
    _io->println("Commands are sent only when you explicitly enter them.");
    prompt();
    return ESP_OK;
}

void AirNginTestConsoleClass::prompt() {
    if (!_io) return;
    _io->print("airngin> ");
    _promptPrinted = true;
}

void AirNginTestConsoleClass::help() {
    if (!_io) return;
    _io->println("\n==================================================");
    _io->println("AIRNGIN ESP32 ARDUINO TEST CONSOLE");
    _io->println("==================================================");
    _io->println("help\nstatus\npublish <key> <value>\nreport <key> <value>");
#if CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE
    _io->println("db set <key> <value>\ndb get <key>\ndb delete <key>");
#endif
    _io->println("sms center <code>\nsms user <code>\npush user <code>");
    _io->println("time utc\ntime tehran\ntime show\ntime off");
    _io->println("mqtt tls status");
    _io->println("prov status\nprov start\nprov cancel");
    _io->println("==================================================");
}

static void atc_print_time(Stream &io, const airngin_time_info_t &t) {
    char b[96];
    snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", (unsigned)t.year,(unsigned)t.month,(unsigned)t.day,
             (unsigned)t.hour,(unsigned)t.minute,(unsigned)t.second);
    io.print("Time source: "); io.println(t.source == AIRNGIN_TIME_SOURCE_TEHRAN ? "TEHRAN" : "UTC");
    io.print("Date/time  : "); io.println(b);
    io.print("Day/week   : "); io.print(t.day_of_week); io.print(" / "); io.println(t.week_number);
    io.print("Day of year: "); io.println(t.day_of_year);
}

void AirNginTestConsoleClass::execute(char *line) {
    airngin_test_command_t c;
    const auto pr = airngin_test_console_parse(line, &c);
    if (pr == ATC_PARSE_EMPTY) return;
    if (pr == ATC_PARSE_UNKNOWN) { _io->print("Unknown command: "); _io->println(c.unknown); return; }
    if (pr != ATC_PARSE_OK) { _io->print("Usage: "); _io->println(c.usage ? c.usage : "help"); return; }

    esp_err_t e = ESP_OK;
    switch (c.kind) {
    case ATC_CMD_HELP: help(); break;
    case ATC_CMD_STATUS: {
        airngin_status_t s = {};
        s.struct_size = sizeof(s);
        e = airngin_get_status(&s);
        if (e != ESP_OK) break;
        _io->print("SDK       : "); _io->println(AIRNGIN_VERSION_STRING);
        _io->print("Lifecycle : "); _io->println(life(s.lifecycle));
        _io->print("Network   : "); _io->println(s.network_connected ? "YES" : "NO");
        _io->print("Clock     : "); _io->println(s.clock_trusted ? "YES" : "NO");
        _io->print("MQTT      : "); _io->println(s.mqtt_ready ? "READY" : (s.mqtt_connected ? "CONNECTED" : "DOWN"));
        _io->print("Ready     : "); _io->println(s.ready ? "YES" : "NO");
        _io->print("Provision : "); _io->println(s.provisioning_active ? "ACTIVE" : "NO");
        break;
    }
    case ATC_CMD_PUBLISH: e = airngin_publish(c.arg1,c.arg2); break;
    case ATC_CMD_REPORT: e = airngin_report(c.arg1,c.arg2); break;
#if CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE
    case ATC_CMD_DB_SET: { uint32_t id=0; e=airngin_database_set(c.arg1,c.arg2,&id); _io->print("Request ID: "); _io->println(id); break; }
    case ATC_CMD_DB_GET: { uint32_t id=0; e=airngin_database_get(c.arg1,&id); _io->print("Request ID: "); _io->println(id); break; }
    case ATC_CMD_DB_DELETE: { uint32_t id=0; e=airngin_database_delete(c.arg1,&id); _io->print("Request ID: "); _io->println(id); break; }
#else
    case ATC_CMD_DB_SET: case ATC_CMD_DB_GET: case ATC_CMD_DB_DELETE: e=ESP_ERR_NOT_SUPPORTED; break;
#endif
    case ATC_CMD_SMS_CENTER: e=airngin_send_sms_to_center(c.arg1); break;
    case ATC_CMD_SMS_USER: e=airngin_send_sms_to_user(c.arg1); break;
    case ATC_CMD_PUSH_USER: e=airngin_send_push_to_user(c.arg1); break;
    case ATC_CMD_TIME_UTC: e=airngin_time_subscribe(AIRNGIN_TIME_SOURCE_UTC); break;
    case ATC_CMD_TIME_TEHRAN: e=airngin_time_subscribe(AIRNGIN_TIME_SOURCE_TEHRAN); break;
    case ATC_CMD_TIME_OFF: e=airngin_time_unsubscribe(); break;
    case ATC_CMD_TIME_SHOW: { airngin_time_info_t t; e=airngin_time_get_latest(&t); if(e==ESP_OK) atc_print_time(*_io,t); break; }
    case ATC_CMD_PROV_STATUS: { airngin_status_t s={}; s.struct_size=sizeof(s); e=airngin_get_status(&s); if(e==ESP_OK){_io->print("Provisioning: ");_io->println(s.provisioning_active?"ACTIVE":"NO");} break; }
    case ATC_CMD_PROV_START: e=airngin_request_provisioning(); break;
    case ATC_CMD_PROV_CANCEL: e=airngin_cancel_provisioning(); break;
    case ATC_CMD_MQTT_TLS_STATUS:
        _io->println("MQTT TLS: ENABLED, port 8883");
#if CONFIG_AIRNGIN_USE_AES
        _io->println("Application protocol: AES");
#else
        _io->println("Application protocol: PLAIN (MQTT is still TLS)");
#endif
        return;
    case ATC_CMD_MQTT_TLS_ON: case ATC_CMD_MQTT_TLS_OFF: e=ESP_ERR_NOT_SUPPORTED; break;
    default: return;
    }
    _io->print("API result: "); _io->print(esp_err_to_name(e)); _io->print(" (0x"); _io->print((uint32_t)e,HEX); _io->println(")");
}

bool AirNginTestConsoleClass::drainEvents() {
    if (!_eventQueue || !_io) return false;
    bool any=false; atc_event_t *e=nullptr;
    while (xQueueReceive(_eventQueue,&e,0)==pdTRUE) {
        any=true; _io->println();
        switch(e->type) {
        case AIRNGIN_EVENT_MQTT_READY: _io->println("EVENT: MQTT_READY"); break;
        case AIRNGIN_EVENT_MQTT_DISCONNECTED: _io->println("EVENT: MQTT_DISCONNECTED"); break;
        case AIRNGIN_EVENT_TIME_RECEIVED: _io->println("EVENT: TIME_RECEIVED"); atc_print_time(*_io,e->data.time); break;
        case AIRNGIN_EVENT_DATABASE_RESULT:
            _io->println("EVENT: DATABASE_RESULT");
            _io->print("Request ID: "); _io->println(e->data.database.request_id);
            _io->print("HTTP      : "); _io->println(e->data.database.http_status);
            _io->print("Success   : "); _io->println(e->data.database.server_success?"YES":"NO");
            _io->print("Key       : "); _io->println(e->data.database.key);
            if(e->data.database.value_len){_io->print("Value     : ");_io->println(e->value);}
            if(e->data.database.message[0]){_io->print("Message   : ");_io->println(e->data.database.message);}
            break;
        case AIRNGIN_EVENT_PROVISIONING_STARTED: _io->println("EVENT: PROVISIONING_STARTED"); break;
        case AIRNGIN_EVENT_PROVISIONING_STOPPED: _io->println("EVENT: PROVISIONING_STOPPED"); break;
        case AIRNGIN_EVENT_PROVISIONING_SUCCESS: _io->println("EVENT: PROVISIONING_SUCCESS"); break;
        default: break;
        }
        free(e);
    }
    return any;
}

void AirNginTestConsoleClass::loop() {
    if (!_io) return;
    if (drainEvents()) prompt();
    while (_io->available()) {
        int ch=_io->read();
        if (ch<0) break;
        if (_discard) { if(ch=='\n'||ch=='\r'){_discard=false;_used=0;prompt();} continue; }
        if (ch=='\n'||ch=='\r') {
            if (_used) { _line[_used]=0; _io->println(); execute(_line); _used=0; }
            prompt();
            continue;
        }
        if (_used+1U >= ATC_LINE_SIZE) { _discard=true; _used=0; _io->println("\nInput too long; discarded."); continue; }
        _line[_used++]=(char)ch;
    }
}

void AirNginTestConsoleClass::handleEvent(const airngin_event_data_t *event) {
    if (!event || !_eventQueue) return;
    size_t valueLen=0;
    if(event->type==AIRNGIN_EVENT_DATABASE_RESULT && event->data.database.value) valueLen=event->data.database.value_len;
    atc_event_t *c=(atc_event_t*)calloc(1,sizeof(*c)+valueLen);
    if(!c) return;
    c->type=event->type; c->error=event->error;
    if(event->type==AIRNGIN_EVENT_TIME_RECEIVED) c->data.time=event->data.time;
    else if(event->type==AIRNGIN_EVENT_MQTT_DISCONNECTED){c->data.mqtt.reason=event->data.mqtt.reason;c->data.mqtt.discarded=event->data.mqtt.tx_discarded;}
    else if(event->type==AIRNGIN_EVENT_DATABASE_RESULT){
        c->data.database.operation=event->data.database.operation;c->data.database.request_id=event->data.database.request_id;
        c->data.database.status=event->data.database.status;c->data.database.http_status=event->data.database.http_status;
        c->data.database.server_success=event->data.database.server_success;c->data.database.value_len=valueLen;
        atc_copy(c->data.database.key,sizeof(c->data.database.key),event->data.database.key);
        atc_copy(c->data.database.message,sizeof(c->data.database.message),event->data.database.message);
        if (valueLen) memcpy(c->value, event->data.database.value, valueLen);
        c->value[valueLen] = 0;
    }
    if(xQueueSend(_eventQueue,&c,0)!=pdTRUE) free(c);
}

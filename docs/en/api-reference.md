# API reference

The Arduino package exposes two levels:

1. The original C API in `airngin.h` (SDK 1.6.0 contract).
2. The Arduino-friendly `AirNginESP32Class` global instance `AirNgin`.

## Lifecycle

### Managed

```cpp
esp_err_t AirNgin.beginManaged(
    const char *serial,
    const char *producerCode,
    airngin_command_handler_t commandHandler = nullptr,
    airngin_event_handler_t eventHandler = nullptr,
    void *userContext = nullptr,
    const char *provisioningLabel = "Device",
    const char *provisioningPassword = CONFIG_AIRNGIN_PROVISIONING_PASSWORD);
```

### External

```cpp
esp_err_t AirNgin.beginExternal(
    const char *serial,
    const char *producerCode,
    const airngin_cloud_credentials_t &cloud,
    airngin_command_handler_t commandHandler = nullptr,
    airngin_event_handler_t eventHandler = nullptr,
    void *userContext = nullptr,
    uint32_t reconnectDelayMs = 3000);
```

### Stop / deinit

```cpp
AirNgin.stop();
AirNgin.end();
```

`end()` does not deinitialize if `stop()` fails; live resources are never force-destroyed.

## Status

```cpp
airngin_status_t s{};
AirNgin.status(s);
bool ready = AirNgin.ready();
airngin_state_t state = AirNgin.state();
```

## Publish / Report

```cpp
AirNgin.publish("key", "value");
AirNgin.report("key", "value");
```

MQTT must be ready. The SDK copies the strings and sends asynchronously.

## Provisioning

```cpp
AirNgin.requestProvisioning();
AirNgin.cancelProvisioning();
```

Managed mode only; provisioning is not supported in External mode.

## External Network

```cpp
AirNgin.setExternalNetworkConnected(true);
AirNgin.replaceExternalCredentials(cloud);
```

The first reports IP usability; the second rotates active Cloud credentials without a full SDK restart.

## Time

```cpp
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_UTC);
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_TEHRAN);
AirNgin.unsubscribeTime();

airngin_time_info_t t;
AirNgin.latestTime(t);
```

Valid packets emit `AIRNGIN_EVENT_TIME_RECEIVED`.

## Notifications

```cpp
AirNgin.notifyCenter("1002");
AirNgin.smsCenter("1002");
AirNgin.smsUser("1002");
AirNgin.pushUser("1002");
```

`ESP_OK` means accepted/queued by the SDK, not confirmed end-user delivery.

## Cloud Database

Enable with `CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1`.

```cpp
uint32_t requestId;
AirNgin.databaseSet("key1", "value", &requestId);
AirNgin.databaseGet("key1", &requestId);
AirNgin.databaseDelete("key1", &requestId);
```

Completion is delivered through `AIRNGIN_EVENT_DATABASE_RESULT`. One request may be outstanding at a time. SET/DELETE are never automatically retried.

## Command callback

```cpp
static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *ctx) {
  return airngin_command_result_set(result, "ok");
}
```

Optionally override the response key:

```cpp
airngin_command_result_set_key(result, "ch1");
```

## Event callback

```cpp
static void onEvent(const airngin_event_data_t *event, void *ctx) {
  switch (event->type) {
    case AIRNGIN_EVENT_MQTT_READY:
      break;
    case AIRNGIN_EVENT_TIME_RECEIVED:
      break;
    case AIRNGIN_EVENT_DATABASE_RESULT:
      break;
    default:
      break;
  }
}
```

Database event strings are callback-lifetime pointers; copy them if needed later.

For exact state/error/ownership rules, `src/airngin.h` is the authoritative API reference.

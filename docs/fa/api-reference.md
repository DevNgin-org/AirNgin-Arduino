# مرجع API

نسخه Arduino دو سطح API دارد:

1. API اصلی C در `airngin.h`؛ همان قرارداد SDK 1.6.0.
2. کلاس ساده‌تر `AirNginESP32Class` و instance سراسری `AirNgin`.

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

### توقف

```cpp
AirNgin.stop();  // RUNNING -> INITIALIZED
AirNgin.end();   // stop + deinit
```

`end()` در صورت خطای `stop()`، deinit را ادامه نمی‌دهد تا resource زنده destroy نشود.

## وضعیت

```cpp
airngin_status_t s{};
AirNgin.status(s);
bool ok = AirNgin.ready();
airngin_state_t state = AirNgin.state();
```

## Publish / Report

```cpp
AirNgin.publish("key", "value");
AirNgin.report("key", "value");
```

برای هر دو، MQTT باید آماده باشد. داده توسط SDK کپی می‌شود و ارسال async است.

## Provisioning

```cpp
AirNgin.requestProvisioning();
AirNgin.cancelProvisioning();
```

فقط Managed Mode. در External Mode، Provisioning پشتیبانی نمی‌شود.

## External Network

```cpp
AirNgin.setExternalNetworkConnected(true);
AirNgin.replaceExternalCredentials(cloud);
```

اولی edge وضعیت IP را به SDK اعلام می‌کند. دومی credentialهای runtime را بدون restart کامل SDK جایگزین می‌کند.

## Time

```cpp
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_UTC);
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_TEHRAN);
AirNgin.unsubscribeTime();

airngin_time_info_t t;
AirNgin.latestTime(t);
```

اولین packet معتبر event `AIRNGIN_EVENT_TIME_RECEIVED` را ایجاد می‌کند.

## Notification

```cpp
AirNgin.notifyCenter("1002");
AirNgin.smsCenter("1002");
AirNgin.smsUser("1002");
AirNgin.pushUser("1002");
```

`ESP_OK` به معنی accepted/queued است، نه تحویل قطعی SMS/Push.

## Cloud Database

ابتدا با `CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1` فعال کنید.

```cpp
uint32_t requestId;
AirNgin.databaseSet("key1", "value", &requestId);
AirNgin.databaseGet("key1", &requestId);
AirNgin.databaseDelete("key1", &requestId);
```

نتیجه در `AIRNGIN_EVENT_DATABASE_RESULT` می‌آید. فقط یک request هم‌زمان مجاز است. SDK برای SET/DELETE retry خودکار انجام نمی‌دهد.

## Command callback

```cpp
static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *ctx) {
  // cmd->key, cmd->value
  return airngin_command_result_set(result, "ok");
}
```

در صورت نیاز key پاسخ را هم می‌توان تغییر داد:

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

به lifetime رشته‌های `AIRNGIN_EVENT_DATABASE_RESULT` توجه کنید: اگر بعد از callback لازم هستند، باید copy شوند.

برای تمام جزئیات state/error/lifetime به commentهای `src/airngin.h` مراجعه کنید؛ آن header مرجع اصلی API است.

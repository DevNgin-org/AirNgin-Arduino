# شروع سریع

در Arduino IDE هستهٔ **esp32 by Espressif Systems 3.3.10** را نصب کنید و بستهٔ ZIP کتابخانه را با **Add .ZIP Library** اضافه کنید. برد سازگار و **Partition Scheme → Huge APP** را انتخاب کنید. از **File → Examples → AirNginESP32SDK → DeviceUptime** شروع کنید. سریال ثبت‌شده و کد تولیدکننده را وارد کنید. برای راه‌اندازی با اپ فعلی، رمز شبکهٔ موقت برد را `00000000` بگذارید. پس از اتصال، برد هر ۳۰ ثانیه زمان کارکرد را با کلید عددی `uptime_seconds` می‌فرستد. واحد این عملیات در پنل ثانیه است. مثال **ManagedBasic** فرمان‌های `on` و `off` کلید `ch1` را روی یک کلید نرم‌افزاری اجرا می‌کند.

## ساده‌ترین حالت: Managed Wi-Fi

```cpp
#include <AirNginESP32SDK.h>
constexpr char setupPassword[] = ""; // set the device-specific password before uploading

static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *) {
  Serial.printf("RX: %s = %s\n", cmd->key, cmd->value);
  return airngin_command_result_set(result, cmd->value);
}

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type == AIRNGIN_EVENT_MQTT_READY)
    Serial.println("AirNgin READY");
}

void setup() {
  Serial.begin(115200);
  esp_err_t err = AirNgin.beginManaged(
      "VTRA0000000000", "VTRA", onCommand, onEvent,
      nullptr, "Device", setupPassword);
  Serial.printf("begin=%s\n", esp_err_to_name(err));
}

void loop() { delay(1000); }
```

اگر Profile معتبر در NVS نباشد، AirNgin یک SoftAP برای Provisioning باز می‌کند. اگر Profile موجود باشد، Wi-Fi، Clock و MQTT را خودش راه‌اندازی می‌کند.

## ارسال داده

```cpp
AirNgin.publish("ch1", "on");
AirNgin.report("temperature", "24.5");
```

همان APIهای C هم در دسترس‌اند:

```c
airngin_publish("ch1", "on");
airngin_report("ch1", "on");
```

## وضعیت

```cpp
airngin_status_t status = AIRNGIN_STATUS_INIT;
if (AirNgin.status(status) == ESP_OK) {
  Serial.println(status.ready ? "READY" : "NOT READY");
}
```

برای مثال کامل `examples/ManagedBasic` را ببینید.

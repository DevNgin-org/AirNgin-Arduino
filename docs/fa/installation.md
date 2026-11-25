# نصب و تنظیم Build

## Arduino IDE

نام کتابخانه در Library Manager پس از ثبت **AirNginESP32SDK** است. در اسکچ جدید از `AirNginESP32SDK.h` استفاده کنید. هدر قبلی `AirNginESP32.h` نیز پشتیبانی می‌شود. برای راه‌اندازی با اپ فعلی AirNgin، مقدار `setupPassword` را در مثال برابر `00000000` قرار دهید. این رمز مربوط به شبکهٔ موقت خود دستگاه است.

1. از Boards Manager، پکیج **ESP32 by Espressif Systems** نسخه 3.3.10 را نصب کنید.
2. از `Sketch > Include Library > Add .ZIP Library` فایل ZIP کتابخانه AirNgin ESP32 SDK را اضافه کنید.
3. برد متناظر با تراشهٔ خود را انتخاب کنید. این نسخه برای ESP32، S2، S3، C3 و C6 ساخته شده است.
4. برای Build کامل (Database/AES/Diagnostic) از Partition بزرگ مثل **Huge APP** استفاده کنید. Buildهای کامل می‌توانند از 1MB عبور کنند.

## تنظیم قابلیت‌های Compile-time در Arduino IDE

Arduino-ESP32 فایل `build_opt.h` را از پوشه Sketch می‌خواند و گزینه‌های آن را به کل Build می‌دهد. مثلاً:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE=1
-DCONFIG_AIRNGIN_USE_AES=0
```

نمونه آماده در `extras/build_opt.example.h` وجود دارد. بعضی Exampleها مثل `CloudDatabase` و `AESExternal` فایل `build_opt.h` مخصوص خودشان را دارند.

## PlatformIO

تنظیم زیر اختیاری است. آزمون انتشار با Arduino CLI انجام می‌شود.

نمونه `extras/platformio.ini.example`:

```ini
[env:esp32-s3-devkitc-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.partitions = huge_app.csv

build_flags =
    -DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
    -DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
```

## تنظیمات پیش‌فرض مهم

| گزینه | پیش‌فرض | توضیح |
|---|---:|---|
| `CONFIG_AIRNGIN_USE_AES` | 0 | AES لایه Application خاموش |
| `CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE` | 0 | Database اختیاری برای کاهش حجم |
| `CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI` | 1 | Managed Wi-Fi در دسترس |
| `CONFIG_AIRNGIN_DIAGNOSTIC_LOGS` | 0 | لاگ تشخیصی |
| `CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS` | 0 | **در Production روشن نکنید** |
| `CONFIG_AIRNGIN_TASK_STACK_SIZE` | 16384 | Stack تسک اصلی AirNgin |
| `CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS` | 3000 | تأخیر reconnect در External Mode |

تنظیمات کامل در `src/AirNginConfig.h` قابل مشاهده است.

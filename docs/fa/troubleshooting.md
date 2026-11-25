# Troubleshooting

## `AIRNGIN_NETWORK_MANAGED_WIFI` با `ESP_ERR_INVALID_STATE` شروع نمی‌شود

احتمالاً Sketch قبل از AirNgin، Wi-Fi driver یا default netif را ساخته است. در Managed Mode از `WiFi.begin()` استفاده نکنید. اگر برنامه باید مالک Wi-Fi باشد، External Mode را انتخاب کنید.

## دستگاه وارد Provisioning می‌شود

- Profile وجود ندارد؛ یا
- Profile با `recovery` علامت‌گذاری شده؛ یا
- از API/دکمه درخواست Provisioning داده شده است.

## MQTT وصل نمی‌شود

بررسی کنید:

- System clock معتبر است.
- Username/Password/ClientId همان مقادیر فعلی AirNgin هستند.
- TLS/8883 مسدود نیست.
- Diagnostics را فعال کنید و CONNACK واقعی را ببینید.

## Build در 1MB جا نمی‌شود

Cloud Database، AES و Diagnostic حجم را زیاد می‌کنند. Partition بزرگ‌تر مثل Huge APP انتخاب کنید.

## Database `ESP_ERR_NOT_SUPPORTED`

در `build_opt.h` اضافه کنید:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
```

در AES build، Database به‌صورت پیش‌فرض block است چون API Database application-layer AES ندارد. فقط با تصمیم امنیتی آگاهانه `CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD=1` را فعال کنید.

## Serial Console command نمی‌گیرد

- Baud = 115200
- Line ending = LF
- `AirNginConsole.loop()` باید مرتب از `loop()` فراخوانی شود.

## `DOWNLOAD(USB/UART0) waiting for download`

ESP32 در Bootloader mode است؛ چیزی از اینترنت دانلود نمی‌شود. BOOT/GPIO0 را رها کنید و Reset/EN بزنید.

## محدودیت Provisioning URI

پورت Arduino از HTTP server مبتنی بر socket خودش استفاده می‌کند؛ بنابراین `GET /set?data=...` به `CONFIG_HTTPD_MAX_URI_LEN` ثابت Core وابسته نیست.

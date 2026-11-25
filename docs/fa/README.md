# مستندات AirNgin ESP32 SDK برای Arduino — نسخه 1.6.1

این کتابخانه، نسخه Arduino-ESP32 از SDK فعلی AirNgin 1.6.0 است. هدف این پورت این است که قراردادهای Cloud، MQTT، Provisioning و APIهای اثبات‌شده تغییر نکنند، ولی استفاده در Arduino IDE و PlatformIO ساده شود.

## از کجا شروع کنم؟

1. [نصب و تنظیم Arduino / PlatformIO](installation.md)
2. [شروع سریع](getting-started.md)
3. یکی از دو مدل شبکه را انتخاب کنید:
   - [Managed Wi-Fi](managed-wifi.md)
   - [External Network](external-network.md)
4. قابلیت‌ها:
   - [Provisioning](provisioning.md)
   - [Publish / Report و دریافت فرمان](data-and-commands.md)
   - [Time، SMS/Push و Cloud Database](cloud-features.md)
   - [TLS، AES و تنظیمات Build](security-and-config.md)
5. برای تست سریع: [Serial Test Console](serial-console.md)
6. [مرجع API](api-reference.md)
7. اگر مشکلی داشتید: [Troubleshooting](troubleshooting.md)

> API سطح پایین C در `airngin.h` همان API نسخه 1.6.0 است. کلاس `AirNgin` فقط یک لایه ساده‌تر برای Sketchهای Arduino است.

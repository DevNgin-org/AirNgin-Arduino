# مستندات AirNgin ESP32 SDK برای Arduino — نسخه 1.6.2

این کتابخانه، نسخه Arduino-ESP32 از SDK فعلی AirNgin 1.6.0 است. هدف این پورت این است که قراردادهای Cloud، MQTT، Provisioning و APIهای اثبات‌شده تغییر نکنند، ولی استفاده در Arduino IDE و PlatformIO ساده شود.

## راه‌اندازی سه مخزن و بردهای پشتیبانی‌شده

| راهنمای راه‌اندازی | برد و محیط | چه چیزی تست شده؟ |
|---|---|---|
| [ESP32 با Arduino یا PlatformIO](getting-started.md) | ESP32، ESP32-S2، ESP32-S3، ESP32-C3 و ESP32-C6 با هستهٔ Arduino-ESP32 3.3.10 | هر ۱۰ مثال نسخهٔ 1.6.2، از جمله DirectNetwork در حالت پیش‌فرض اترنت، برای هر پنج خانواده کامپایل شدند. روی یک ESP32-S3 واقعی، مثال DeviceUptime بعد از خاموش‌روشن شدن با تنظیمات ذخیره‌شده به وای‌فای وصل شد، ساعت را تنظیم کرد و به MQTT READY رسید. |
| [ESP32-S3 با ESP-IDF](https://github.com/DevNgin-org/airngin-esp-idf-sdk/blob/main/docs/fa/getting-started.md) | ESP32-S3 با ESP-IDF 5.5.x | ساخت پروژه و آزمون‌های نرم‌افزاری برای پیکربندی‌های مشخص ثبت شده‌اند. وضعیت هر قابلیت در [گزارش آزمون SDK](https://github.com/DevNgin-org/airngin-esp-idf-sdk/blob/main/VALIDATION.md) آمده است. |
| [STM32 با اترنت](https://github.com/DevNgin-org/AirNginSTM32LAN#readme) | NUCLEO-F401RE با شیلد W5100 | مثال LanUptime برای F401RE کامپایل شده است. کشف برد از اپ و رسیدن داده به پنل روی سخت‌افزار واقعی هنوز تأیید نشده است. |

نمونهٔ جدید [DirectNetwork](../../examples/DirectNetwork/DirectNetwork.ino) در هر دو حالت کابل و وای‌فای دستی برای ESP32-S3 کامپایل شده است. جفت‌سازی این نمونه با اپ روی برد واقعی هنوز بررسی نشده است. [جزئیات تست‌های Arduino](../../VALIDATION.md)

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

## کدام نمونه را باز کنم؟

| نمونه | چه زمانی استفاده کنم؟ |
|---|---|
| [ManagedBasic](../../examples/ManagedBasic/ManagedBasic.ino) | برای راه‌اندازی با اپ از طریق وای‌فای موقت برد (SoftAP) و آزمایش فرمان روشن و خاموش روی کلید نرم‌افزاری `ch1`. |
| [DirectNetwork](../../examples/DirectNetwork/DirectNetwork.ino) | برای راه‌اندازی با اپ وقتی برد با کابل LAN یا وای‌فای دستی به شبکه وصل می‌شود. بعد از گرفتن IP، اپ روی همان شبکه برد را با mDNS پیدا می‌کند و تنظیمات پروژه را می‌فرستد. |
| [ExternalWiFi](../../examples/ExternalWiFi/ExternalWiFi.ino) | وقتی برنامهٔ خودتان اتصال وای‌فای را مدیریت می‌کند و مشخصات وای‌فای و MQTT را در کد وارد می‌کنید. |
| [DeviceUptime](../../examples/DeviceUptime/DeviceUptime.ino) | برای تست ارسال داده بدون سنسور. برد هر ۳۰ ثانیه زمان کارکردش را با کلید `uptime_seconds` می‌فرستد. |
| [ManagedProvisionButton](../../examples/ManagedProvisionButton/ManagedProvisionButton.ino) | برای ورود به حالت راه‌اندازی با نگه‌داشتن دکمهٔ فیزیکی به مدت ۱۰ ثانیه. نگه‌داشتن دوبارهٔ دکمه برای ۱۰ ثانیه این حالت را لغو می‌کند. |
| [AESExternal](../../examples/AESExternal/AESExternal.ino) | برای پروژه‌ای که علاوه بر MQTT/TLS، پیام‌ها را با کلید AES-256-GCM هم رمز می‌کند. مشخصات اتصال و کلید را در نمونه وارد می‌کنید. |
| [TimeFeed](../../examples/TimeFeed/TimeFeed.ino) | برای دریافت ساعت تهران از AirNgin و نمایش آن در Serial Monitor. |
| [Notifications](../../examples/Notifications/Notifications.ino) | برای ارسال پیامک به مرکز یا مالک، یا اعلان پوش به مالک، با کد پیام تأییدشده در پنل. |
| [CloudDatabase](../../examples/CloudDatabase/CloudDatabase.ino) | برای نوشتن، خواندن و حذف یک کلید آزمایشی در دیتابیس ابری از طریق HTTPS با فرمان‌های Serial Monitor. |
| [SerialTestConsole](../../examples/SerialTestConsole/SerialTestConsole.ino) | برای تست فرمان‌های SDK و دیدن رویدادهای برد از طریق کنسول سریال هنگام توسعه. |

در **DirectNetwork**، سریال ثبت‌شده و کد تولیدکننده را در `deviceSerial` و `producer` وارد کنید. برای کابل، مقدار پیش‌فرض `Connection::Ethernet` را نگه دارید. برای وای‌فای دستی، `Connection::ManualWiFi` را انتخاب کنید و `wifiSSID` و `wifiPassword` را پر کنید. در اولین راه‌اندازی، اپ از لحظه‌ای که برد IP می‌گیرد سه دقیقه فرصت دارد تنظیمات پروژه را بفرستد. برای باز کردن دوبارهٔ این فرصت، حرف `p` را در Serial Monitor بفرستید.

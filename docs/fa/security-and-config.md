# TLS، AES و تنظیمات امنیتی

## TLS و AES یکی نیستند

AirNgin همیشه MQTT اصلی را روی TLS و پورت 8883 اجرا می‌کند. گزینه PLAIN فقط AES لایه Application را خاموش می‌کند؛ ارتباط شبکه همچنان TLS است.

```text
PLAIN = MQTT/TLS + JSON عادی AirNgin
AES   = MQTT/TLS + AES-256-GCM روی payload AirNgin
```

## فعال کردن AES

در `build_opt.h`:

```text
-DCONFIG_AIRNGIN_USE_AES=1
```

AES Build به `encryptionKey` معتبر (Base64 از دقیقاً 32 بایت) نیاز دارد. در External mode این مقدار در `airngin_cloud_credentials_t.encryption_key` است؛ در Managed mode از Provisioning می‌آید.

## Diagnostic

```text
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS=0
```

در PLAIN، payload دریافتی برای Debug قابل چاپ است. در AES، plaintext decrypt شده فقط وقتی secret diagnostics عمداً فعال شده باشد چاپ می‌شود. AES key هیچ‌وقت چاپ نمی‌شود.

## Certificateها در Arduino port

- MQTT: Root CA پروتکل AirNgin داخل Library embed شده است.
- Cloud Database: ISRG Root X1 داخل Library embed شده و hostname verification فعال است.

TLS را با `setInsecure()` یا skip verification ضعیف نکنید.

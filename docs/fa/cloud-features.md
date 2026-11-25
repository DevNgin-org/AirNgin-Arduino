# قابلیت‌های Cloud: Time، پیامک/Push و Database

## Time

```cpp
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_TEHRAN);
```

آخرین زمان:

```cpp
airngin_time_info_t t;
if (AirNgin.latestTime(t) == ESP_OK) {
  Serial.printf("%04u/%02u/%02u %02u:%02u:%02u\n",
                t.year,t.month,t.day,t.hour,t.minute,t.second);
}
```

Time یک feed دوره‌ای MQTT است و جای ساعت اولیه TLS/SNTP را نمی‌گیرد.

مستند رسمی: https://docs.airngin.com/fa/docs/tutorial-time/getting-started

## SMS / Push

```cpp
AirNgin.smsCenter("1002");
AirNgin.smsUser("1002");
AirNgin.pushUser("1002");
```

- `20` = SMS
- `30` = Push
- `ESP_OK` فقط یعنی درخواست توسط SDK پذیرفته شده؛ تحویل نهایی را تأیید نمی‌کند.
- سرویس ممکن است از Wallet پروژه هزینه کم کند؛ SDK هیچ پیام خودکاری نمی‌فرستد.

مستند رسمی: https://docs.airngin.com/fa/docs/tutorial-sms-notfictaion-gateway/send-message-to-own/

## Cloud Database

Cloud Database **MQTT نیست**؛ HTTPS است و باید با گزینه زیر فعال شود:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
```

APIها:

```cpp
uint32_t id;
AirNgin.databaseSet("testKey1", "hello", &id);
AirNgin.databaseGet("testKey1", &id);
AirNgin.databaseDelete("testKey1", &id);
```

نتیجه Async است و از `AIRNGIN_EVENT_DATABASE_RESULT` می‌آید. رشته‌های event فقط تا پایان callback معتبرند.

Database از `https://app.airngin.com/api/v1.0/DeviceCloud` استفاده می‌کند و برای TLS یک Trust Anchor داخلی دارد. MQTT_READY برای Database لازم نیست.

مستند رسمی: https://docs.airngin.com/fa/docs/tutorial-database/getting-started

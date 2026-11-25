# Cloud features: Time, SMS/Push and Database

## Time

```cpp
AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_TEHRAN);
```

Latest packet:

```cpp
airngin_time_info_t t;
if (AirNgin.latestTime(t) == ESP_OK) {
  Serial.printf("%04u/%02u/%02u %02u:%02u:%02u\n",
                t.year,t.month,t.day,t.hour,t.minute,t.second);
}
```

The Time topic is a periodic MQTT feed; it does not replace the bootstrap SNTP/system clock required for TLS.

Official docs: https://docs.airngin.com/fa/docs/tutorial-time/getting-started

## SMS / Push

```cpp
AirNgin.smsCenter("1002");
AirNgin.smsUser("1002");
AirNgin.pushUser("1002");
```

- `20` = SMS
- `30` = Push
- `ESP_OK` means SDK acceptance, not confirmed delivery.
- Messaging can consume project wallet credit. The SDK never auto-sends a message.

Official docs: https://docs.airngin.com/fa/docs/tutorial-sms-notfictaion-gateway/send-message-to-own/

## Cloud Database

Cloud Database is **HTTPS, not MQTT**. Enable it at build time:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
```

APIs:

```cpp
uint32_t id;
AirNgin.databaseSet("testKey1", "hello", &id);
AirNgin.databaseGet("testKey1", &id);
AirNgin.databaseDelete("testKey1", &id);
```

Results are asynchronous through `AIRNGIN_EVENT_DATABASE_RESULT`; event string pointers are valid only during the callback.

The API uses `https://app.airngin.com/api/v1.0/DeviceCloud` and the Arduino port embeds its HTTPS trust anchor. MQTT_READY is not a Database prerequisite.

Official docs: https://docs.airngin.com/fa/docs/tutorial-database/getting-started

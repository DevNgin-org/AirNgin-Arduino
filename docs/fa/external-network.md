# External Network

در External Mode، Arduino Sketch مالک اتصال شبکه و ساعت است و AirNgin فقط MQTT/TLS و پروتکل Cloud را مدیریت می‌کند.

```cpp
#include <WiFi.h>
#include <AirNginESP32.h>

static const airngin_cloud_credentials_t cloud = {
  .project_code = "YOUR_16_CHAR_PROJECT",
  .broker_host = "mqtt.airngin.com",
  .username = "YOUR_MQTT_USER",
  .password = "YOUR_MQTT_PASS",
  .client_id = "YOUR_SERVER_ISSUED_CLIENT_ID",
  .encryption_key = nullptr,
};

void setup() {
  WiFi.begin("SSID", "PASSWORD");
  configTime(0, 0, "time.cloudflare.com");
  AirNgin.beginExternal("VTRA0000000000", "VTRA", cloud);
}

void loop() {
  static bool old = false;
  bool now = WiFi.status() == WL_CONNECTED;
  if (now != old) {
    old = now;
    AirNgin.setExternalNetworkConnected(now);
  }
}
```

نکات مهم:

- AirNgin در این حالت Wi-Fi، NVS Profile، Provisioning و SNTP را لمس نمی‌کند.
- برنامه باید System Clock معتبر بسازد؛ فقط داشتن IP برای TLS کافی نیست.
- Credentialهای Cloud از ساختار `airngin_cloud_credentials_t` می‌آیند.
- برای rotation می‌توانید از `AirNgin.replaceExternalCredentials()` استفاده کنید.

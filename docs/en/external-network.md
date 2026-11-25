# External Network

In External mode the Arduino sketch owns IP connectivity and time; AirNgin owns MQTT/TLS and the AirNgin protocol.

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

Notes:

- In this mode AirNgin does not own Wi-Fi, NVS profile storage, provisioning, or SNTP.
- Your application must maintain a trusted system clock; IP connectivity alone is not enough for TLS.
- Cloud identity comes from `airngin_cloud_credentials_t`.
- Use `AirNgin.replaceExternalCredentials()` for runtime credential replacement.

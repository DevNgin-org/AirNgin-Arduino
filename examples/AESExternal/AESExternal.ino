/*
 * This example includes build_opt.h with CONFIG_AIRNGIN_USE_AES=1.
 * PlatformIO users can set the same macro in build_flags. AES is application-layer security ON TOP OF MQTT/TLS.
 */
#include <WiFi.h>
#include <AirNginESP32.h>

void setup() {
  Serial.begin(115200);
  WiFi.begin("YOUR_WIFI_SSID", "YOUR_WIFI_PASSWORD");
  configTime(0, 0, "time.cloudflare.com");

  static const airngin_cloud_credentials_t cloud = {
    .project_code = "12345678901234XN",
    .broker_host = "mqtt.airngin.com",
    .username = "YOUR_MQTT_USER",
    .password = "YOUR_MQTT_PASS",
    .client_id = "DVC-12345678901234XN-VTRA0000000000-10",
    .encryption_key = "REPLACE_WITH_BASE64_OF_EXACTLY_32_BYTES",
  };
  esp_err_t err = AirNgin.beginExternal("VTRA0000000000", "VTRA", cloud);
  Serial.printf("beginExternal AES: %s\n", esp_err_to_name(err));
}

void loop() {
  static bool last = false;
  bool now = WiFi.status() == WL_CONNECTED;
  if (now != last && AirNgin.setExternalNetworkConnected(now) == ESP_OK) {
    last = now;
  }
  delay(100);
}

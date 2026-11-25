#include <WiFi.h>
#include <AirNginESP32.h>

const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASS = "YOUR_WIFI_PASSWORD";

static bool lastConnected = false;

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type == AIRNGIN_EVENT_MQTT_READY) Serial.println("AirNgin READY");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");

  static const airngin_cloud_credentials_t cloud = {
    .project_code = "12345678901234XN",
    .broker_host = "mqtt.airngin.com",
    .username = "YOUR_MQTT_USER",
    .password = "YOUR_MQTT_PASS",
    .client_id = "DVC-12345678901234XN-VTRA0000000000-10",
    .encryption_key = nullptr,
  };

  esp_err_t err = AirNgin.beginExternal("VTRA0000000000", "VTRA", cloud, nullptr, onEvent);
  Serial.printf("AirNgin beginExternal: %s\n", esp_err_to_name(err));
}

void loop() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected != lastConnected) {
    if (AirNgin.setExternalNetworkConnected(connected) == ESP_OK) {
      lastConnected = connected;
      Serial.printf("Network: %s\n", connected ? "UP" : "DOWN");
    }
  }
  delay(100);
}

#include <AirNginESP32SDK.h>
#include <esp_timer.h>

// Enter the registered device serial, its producer code and a unique setup password.
constexpr char deviceSerial[] = "VTRA0000000000";
constexpr char producerCode[] = "VTRA";
constexpr char setupPassword[] = "";
constexpr uint32_t reportIntervalMs = 30000;
uint32_t lastReportMs = 0;

void setup() {
  Serial.begin(115200);
  if (sizeof(setupPassword) - 1 < 8 || sizeof(setupPassword) - 1 > 63) {
    Serial.println("Set a device-specific setup password (8 to 63 characters).");
  }
  esp_err_t err = AirNgin.beginManaged(deviceSerial, producerCode, nullptr, nullptr,
                                       nullptr, "Device", setupPassword);
  Serial.printf("AirNgin startup: %s\n", esp_err_to_name(err));
}

void loop() {
  const uint32_t now = millis();
  if (AirNgin.ready() && now - lastReportMs >= reportIntervalMs) {
    lastReportMs = now;
    char seconds[24];
    snprintf(seconds, sizeof(seconds), "%lld", (long long)(esp_timer_get_time() / 1000000));
    esp_err_t err = AirNgin.publish("uptime_seconds", seconds);
    Serial.printf("uptime_seconds=%s, queue: %s\n", seconds, esp_err_to_name(err));
  }
  delay(10);
}

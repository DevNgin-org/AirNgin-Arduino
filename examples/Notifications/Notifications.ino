#include <AirNginESP32.h>

// Assign the setup password delivered with this device before uploading.
constexpr char setupPassword[] = "";

void setup() {
  Serial.begin(115200);
  if (!setupPassword[0]) {
    Serial.println("Set setupPassword to 8-63 printable ASCII characters for this device.");
  }
  esp_err_t started = AirNgin.beginManaged("VTRA0000000000", "VTRA", nullptr, nullptr, nullptr, "Device", setupPassword);
  if (started != ESP_OK) {
    Serial.printf("AirNgin startup: %s\n", esp_err_to_name(started));
    return;
  }
  Serial.println("After MQTT is ready, send one of these in Serial Monitor:");
  Serial.println("c = SMS/message to producer/center");
  Serial.println("s = SMS to device owner");
  Serial.println("p = Push notification to device owner");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  const char c = Serial.read();
  const char *code = "1002"; // must already exist/approved in your AirNgin panel
  esp_err_t err = ESP_ERR_INVALID_ARG;
  if (c == 'c') err = AirNgin.smsCenter(code);
  else if (c == 's') err = AirNgin.smsUser(code);
  else if (c == 'p') err = AirNgin.pushUser(code);
  else return;
  Serial.printf("Request accepted by SDK: %s (delivery is not confirmed here)\n", esp_err_to_name(err));
}

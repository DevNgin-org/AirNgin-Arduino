#include <AirNginESP32.h>
#include <AirNginProvisionButton.h>

// Select a free input on your board and wire the button between that GPIO and GND.
// -1 leaves the pin untouched on boards with flash or PSRAM on GPIO16.
constexpr int provisionButtonPin = -1;
AirNginProvisionButton provisionButton;

// Assign the setup password delivered with this device before uploading.
constexpr char setupPassword[] = "";

void setup() {
  Serial.begin(115200);
  if (!setupPassword[0]) {
    Serial.println("Set setupPassword to 8-63 printable ASCII characters for this device.");
  }
  delay(500);
  esp_err_t err = AirNgin.beginManaged("VTRA0000000000", "VTRA", nullptr, nullptr, nullptr, "Device", setupPassword);
  Serial.printf("AirNgin begin: %s\n", esp_err_to_name(err));

  // Active LOW. Hold the configured input for 10 seconds to enter provisioning.
  // Release: provisioning stays open. Hold another 10 seconds: cancel.
  provisionButton.begin(provisionButtonPin, 10000, 50);
}

void loop() {
  provisionButton.update();
  delay(10);
}

#include <AirNginESP32.h>
#include <AirNginTestConsole.h>
#include <AirNginProvisionButton.h>

// Select a free input on your board and wire the button between that GPIO and GND.
// -1 leaves the pin untouched on boards with flash or PSRAM on GPIO16.
constexpr int provisionButtonPin = -1;
AirNginProvisionButton provisionButton;

static esp_err_t onCommand(const airngin_command_t *cmd, airngin_command_result_t *result, void *) {
  Serial.printf("Command %s=%s\n", cmd->key, cmd->value);
  return airngin_command_result_set(result, cmd->value);
}

static void onEvent(const airngin_event_data_t *event, void *) {
  AirNginConsole.handleEvent(event);
}

// Assign the setup password delivered with this device before uploading.
constexpr char setupPassword[] = "";

void setup() {
  Serial.begin(115200);
  if (!setupPassword[0]) {
    Serial.println("Set setupPassword to 8-63 printable ASCII characters for this device.");
  }
  delay(500);
  esp_err_t started = AirNgin.beginManaged("VTRA0000000000", "VTRA", onCommand, onEvent, nullptr, "Device", setupPassword);
  if (started != ESP_OK) {
    Serial.printf("AirNgin startup: %s\n", esp_err_to_name(started));
    return;
  }
  provisionButton.begin(provisionButtonPin, 10000, 50);
  AirNginConsole.begin(Serial);
}

void loop() {
  provisionButton.update();
  AirNginConsole.loop();
  delay(5);
}

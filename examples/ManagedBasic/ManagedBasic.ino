#include <AirNginESP32.h>

static esp_err_t onCommand(const airngin_command_t *cmd, airngin_command_result_t *result, void *) {
  // A logical switch in RAM lets this example run without board-specific wiring.
  static bool switchOn = false;
  if (strcmp(cmd->key, "ch1") != 0) return ESP_ERR_NOT_SUPPORTED;
  if (strcmp(cmd->value, "on") == 0) switchOn = true;
  else if (strcmp(cmd->value, "off") == 0) switchOn = false;
  else return ESP_ERR_INVALID_ARG;
  Serial.printf("ch1 = %s\n", switchOn ? "on" : "off");
  return airngin_command_result_set(result, switchOn ? "on" : "off");
}

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type == AIRNGIN_EVENT_MQTT_READY) Serial.println("AirNgin MQTT READY");
  if (event->type == AIRNGIN_EVENT_PROVISIONING_STARTED) Serial.println("Provisioning AP opened");
  if (event->type == AIRNGIN_EVENT_PROVISIONING_SUCCESS) Serial.println("Provisioning saved; restarting soon");
}

// Assign the setup password delivered with this device before uploading.
constexpr char setupPassword[] = "";

void setup() {
  Serial.begin(115200);
  if (!setupPassword[0]) {
    Serial.println("Set setupPassword to 8-63 printable ASCII characters for this device.");
  }
  delay(500);

  // Managed mode owns Wi-Fi. Do NOT call WiFi.begin() in this mode.
  esp_err_t err = AirNgin.beginManaged(
      "VTRA0000000000",   // replace with your 14-char device serial
      "VTRA",
      onCommand,
      onEvent,
      nullptr,
      "Device",
      setupPassword);

  Serial.printf("AirNgin begin: %s\n", esp_err_to_name(err));
}

void loop() {
  // AirNgin runs in FreeRTOS tasks; your application loop stays free.
  delay(1000);
}

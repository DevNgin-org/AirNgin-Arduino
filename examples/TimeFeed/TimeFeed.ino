#include <AirNginESP32.h>

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type == AIRNGIN_EVENT_TIME_RECEIVED) {
    const auto &t = event->data.time;
    Serial.printf("AirNgin time: %04u-%02u-%02u %02u:%02u:%02u\n",
                  t.year, t.month, t.day, t.hour, t.minute, t.second);
  }
}

// Assign the setup password delivered with this device before uploading.
constexpr char setupPassword[] = "";

void setup() {
  Serial.begin(115200);
  if (!setupPassword[0]) {
    Serial.println("Set setupPassword to 8-63 printable ASCII characters for this device.");
  }
  esp_err_t started = AirNgin.beginManaged("VTRA0000000000", "VTRA", nullptr, onEvent, nullptr, "Device", setupPassword);
  if (started != ESP_OK) {
    Serial.printf("AirNgin startup: %s\n", esp_err_to_name(started));
    return;
  }
  // The wish is remembered and automatically applied after MQTT is ready/reconnects.
  AirNgin.subscribeTime(AIRNGIN_TIME_SOURCE_TEHRAN);
}

void loop() { delay(1000); }

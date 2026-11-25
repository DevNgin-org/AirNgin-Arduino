#include <AirNginESP32.h>

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type != AIRNGIN_EVENT_DATABASE_RESULT) return;
  const auto &db = event->data.database;
  Serial.printf("DB result request=%u op=%d status=%s http=%d success=%d key=%s\n",
                (unsigned)db.request_id, (int)db.operation, esp_err_to_name(db.status),
                db.http_status, db.server_success, db.key ? db.key : "");
  if (db.value) Serial.printf("value=%.*s\n", (int)db.value_len, db.value);
  if (db.message) Serial.printf("message=%s\n", db.message);
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
  Serial.println("Commands: s=set, g=get, d=delete testKey1");
}

void loop() {
  if (!Serial.available()) { delay(20); return; }
  const char c = Serial.read();
  uint32_t id = 0;
  esp_err_t err = ESP_ERR_INVALID_ARG;
  if (c == 's') err = AirNgin.databaseSet("testKey1", "hello-airngin-db", &id);
  else if (c == 'g') err = AirNgin.databaseGet("testKey1", &id);
  else if (c == 'd') err = AirNgin.databaseDelete("testKey1", &id);
  else return;
  Serial.printf("DB API: %s request=%u\n", esp_err_to_name(err), (unsigned)id);
}

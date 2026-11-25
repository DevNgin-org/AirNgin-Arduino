# Quick start

For a first reading, install the library ZIP and open **DeviceUptime** under File → Examples → AirNginESP32SDK. Set the registered serial and producer code. For provisioning with the current mobile app, set the setup AP password to `00000000`. After provisioning and MQTT connection, the sketch publishes `uptime_seconds` every 30 seconds.

## Simplest path: Managed Wi-Fi

```cpp
#include <AirNginESP32SDK.h>
constexpr char setupPassword[] = ""; // set the device-specific password before uploading

static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *) {
  Serial.printf("RX: %s = %s\n", cmd->key, cmd->value);
  return airngin_command_result_set(result, cmd->value);
}

static void onEvent(const airngin_event_data_t *event, void *) {
  if (event->type == AIRNGIN_EVENT_MQTT_READY)
    Serial.println("AirNgin READY");
}

void setup() {
  Serial.begin(115200);
  esp_err_t err = AirNgin.beginManaged(
      "VTRA0000000000", "VTRA", onCommand, onEvent,
      nullptr, "Device", setupPassword);
  Serial.printf("begin=%s\n", esp_err_to_name(err));
}

void loop() { delay(1000); }
```

Without a valid NVS profile AirNgin opens its provisioning SoftAP. With a profile, AirNgin owns Wi-Fi, time acquisition and MQTT startup.

## Send data

```cpp
AirNgin.publish("ch1", "on");
AirNgin.report("temperature", "24.5");
```

The original C API is also available:

```c
airngin_publish("ch1", "on");
airngin_report("ch1", "on");
```

## Status

```cpp
airngin_status_t status = AIRNGIN_STATUS_INIT;
if (AirNgin.status(status) == ESP_OK) {
  Serial.println(status.ready ? "READY" : "NOT READY");
}
```

See `examples/ManagedBasic` for a complete sketch.

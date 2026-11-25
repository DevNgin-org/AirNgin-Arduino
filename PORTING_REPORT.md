# AirNgin SDK 1.6.0 → Arduino-ESP32 Porting Report

## Feature matrix

| Capability | Arduino port | Notes |
|---|---|---|
| Lifecycle / status | Ported | Original C API retained + C++ facade |
| Managed Wi-Fi | Ported | Uses ESP-IDF Wi-Fi APIs exposed by Arduino-ESP32; application must not pre-own Wi-Fi |
| External network | Ported | Designed for Arduino `WiFi`, Ethernet, PPP, etc. |
| Provisioning AP | Ported | Same profile/session behavior; socket HTTP adapter replaces `esp_http_server` |
| the selected GPIO 10 s toggle | Ported | `AirNginProvisionButton`, cooperative `loop()` helper |
| MQTT 3.1.1 / TLS 8883 | Ported | Same `esp-mqtt` transport and AirNgin root CA |
| PLAIN mode | Ported | Same wire format |
| AES-256-GCM | Ported | Same implementation and compile-time mode |
| Publish / Report | Ported | Same API and queue architecture |
| Command RX / response | Ported | Same C callback contract |
| Time feed | Ported | UTC / Tehran |
| SMS / Push | Ported | Same APIs / topic builder |
| Cloud Database HTTPS | Ported | `esp_http_client`; embedded ISRG Root X1 |
| Diagnostics | Ported | Compile-time macros through Arduino build options |
| Unified test console | Ported | Arduino `Stream`, no stdin task |

## Public API compatibility

`src/airngin.h` is byte-for-byte equivalent to the ESP-IDF 1.6.0 public header except for including `AirNginConfig.h` so Arduino builds receive the configuration macros that Kconfig used to provide.

## Packaging changes

ESP-IDF packaging (`CMakeLists.txt`, Kconfig, `idf_component.yml`) is not part of the Arduino library. Arduino uses:

- `library.properties`
- `library.json` (PlatformIO metadata)
- `src/`
- `examples/`
- sketch `build_opt.h` or PlatformIO `build_flags`

## Provisioning adaptation

The ESP-IDF application configured a very large `CONFIG_HTTPD_MAX_URI_LEN` for the provisioning GET endpoint. A normal Arduino-ESP32 install uses prebuilt ESP-IDF libraries, so that sdkconfig value cannot be changed per sketch. The Arduino port therefore uses a bounded TCP socket HTTP adapter on the provisioning AP while keeping:

- `192.168.1.1`
- `GET /health`
- `GET /set?data=...`
- `POST /set`
- the same query decoder / JSON parser / validation
- the same NVS profile format
- 120-second session behavior
- save/readback/restart behavior

## Configuration mapping

Every `CONFIG_AIRNGIN_*` option used by the core has a safe default in `AirNginConfig.h`. Arduino IDE users should use sketch `build_opt.h`; PlatformIO users should use `build_flags`.

## Build verification

See [validation results](VALIDATION.md).

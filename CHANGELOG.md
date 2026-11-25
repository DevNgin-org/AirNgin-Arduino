# Changelog

## Arduino library 1.6.2

- Add DirectNetwork, a standalone example for Ethernet or manual Wi-Fi with local discovery and provisioning from the AirNgin mobile app.
- Add a bilingual guide to all ten examples, with setup links and board validation details.

## Arduino library 1.6.1

- Add the DeviceUptime telemetry sketch and multi-board compile matrix.
- Require users to configure the setup password in managed examples and the physical pin for button examples.
- Package the 1.6.0 device protocol with ZIP installation, bilingual instructions and release checks.

## 1.6.0-arduino-port

Arduino-ESP32 source port of AirNgin ESP-IDF SDK 1.6.0.

### Added

- Standard Arduino library layout (`library.properties`, `src`, `examples`).
- `AirNginESP32Class` facade with global `AirNgin` instance.
- `AirNginProvisionButton` 10-second active-low helper.
- `AirNginTestConsole` Arduino `Stream` console.
- Arduino IDE `build_opt.h` and PlatformIO build-flag configuration model.
- Managed, External, AES, Time, Notification, Database and Console examples.
- Persian and English Arduino documentation.

### Adapted from ESP-IDF component packaging

- Kconfig defaults → `AirNginConfig.h` compile-time defaults.
- MQTT linker-embedded PEM → C-array embedded trust anchor.
- DeviceCloud certificate bundle → embedded ISRG Root X1.
- Provisioning `esp_http_server` → bounded BSD-socket HTTP adapter to preserve long `/set?data=...` compatibility with prebuilt Arduino cores.
- Root test-app FreeRTOS input/button helpers → Arduino `loop()` helpers.

### Intentionally unchanged

- AirNgin C API and version string 1.6.0.
- MQTT topics/wire JSON.
- TLS/8883 behavior.
- AES protocol.
- NVS profile format.
- Managed vs External ownership rules.
- Cloud Database endpoints/payloads.

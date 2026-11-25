# Arduino Port Notes

This package was derived from the AirNgin ESP-IDF SDK 1.6.0 with the protocol/lifecycle core kept in C.

## Main adaptations

| ESP-IDF SDK | Arduino port |
|---|---|
| Kconfig | `AirNginConfig.h`, sketch `build_opt.h`, or PlatformIO `build_flags` |
| Component CMake | Standard Arduino `src/` library layout |
| Embedded MQTT PEM linker symbol | C header containing the same trust anchor |
| ESP-IDF certificate bundle for DeviceCloud | Embedded ISRG Root X1 trust anchor |
| `esp_http_server` provisioning | Small BSD-socket HTTP server for long compatible requests |
| Root hardware app button task | `AirNginProvisionButton` helper called from Arduino `loop()` |
| Root stdin task | `AirNginTestConsole` on Arduino `Stream` |

## Intentionally unchanged

- Public C API and SDK version (1.6.0)
- MQTT topics and JSON wire format
- MQTT/TLS port and certificate/hostname verification
- AES-256-GCM format
- NVS profile format/namespace
- 120-second provisioning session
- Managed/external ownership model
- Cloud Database endpoint/payloads
- Time and notification contracts

## Build verification

See [validation results](../VALIDATION.md).

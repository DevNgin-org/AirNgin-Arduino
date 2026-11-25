# AirNginESP32 Arduino Port — Validation Status

## Completed in the porting environment

- Source inventory and API comparison against AirNgin ESP-IDF SDK 1.6.0.
- Original public C API retained in `src/airngin.h`.
- Arduino C++ facade syntax-checked with GCC in C++17 mode and `-Wall -Wextra -Werror` using minimal Arduino/FreeRTOS stubs.
- All supplied `.ino` examples syntax-checked in C++17 mode using Arduino/WiFi stubs.
- Unified console parser smoke-tested on the host (`publish`, quoted DB values, notification, Time).
- Original host-test subset passed against the Arduino-port sources:
  - identity
  - provisioning query boundaries
  - provisioning session FSM
  - provisioning FSM
  - topic builder
  - diagnostic text helper
- Local include audit: no unresolved quoted internal include.
- Markdown relative-link audit: no broken internal links.
- Known real project credentials/serials used during SDK development were scanned and are not present in this package.

## Arduino firmware builds

Arduino CLI 1.3.1, Espressif core 3.3.10, 4 MB flash, Huge APP partition. The generic Dev Module FQBNs are `esp32:esp32:esp32`, `esp32:esp32:esp32s2`, `esp32:esp32:esp32s3`, `esp32:esp32:esp32c3` and `esp32:esp32:esp32c6`. The release workflow compiles every example sketch for each target, including the AES and HTTPS database configurations.

Local results on 2026-11-25 using the Dev Module defaults with the partition above:

| Target | Sketches built and linked | Largest sketch (bytes) |
| --- | ---: | ---: |
| `esp32` | 9/9 | 1,093,801 |
| `esp32s2` | 9/9 | 1,026,331 |
| `esp32s3` | 9/9 | 1,058,008 |
| `esp32c3` | 9/9 | 1,151,395 |
| `esp32c6` | 9/9 | 1,165,290 |

The ZIP was installed into a clean Arduino sketchbook. DeviceUptime and CloudDatabase were also compiled directly from that installation on all five targets. The host fixtures and runner live in the companion ESP-IDF SDK development tree.

The host regression suite for the Arduino sources covers device identity, query decoding, provisioning transitions, diagnostics, MQTT topic formation, the command/report JSON protocol, AES fixtures, and the PLAIN/AES profile format.

## DirectNetwork example

[GitHub Actions](https://github.com/DevNgin-org/AirNgin-Arduino/actions/runs/45597719631) compiled all ten sketches on all five targets, with DirectNetwork in its default Ethernet mode. In local tests, DirectNetwork also compiled and linked for the ESP32-S3 Dev Module with Arduino-ESP32 3.3.10 and Huge APP in both configurations: Ethernet (W5500) and manual Wi-Fi. Physical discovery, mobile pairing and cloud delivery have yet to be verified for this sketch. The nine-example matrix above describes the v1.6.1 release.

## ESP32-S3 bench test

On a 16 MB ESP32-S3 with 8 MB PSRAM, a DeviceUptime test build retained its provisioned profile after restart, joined Wi-Fi, synchronized its clock and reached MQTT READY. Uptime reports at 30 and 60 seconds were accepted by the SDK queue. Dashboard receipt was not verified.

## Remaining hardware checks

Repeat the following on each product configuration. The S3 observations above cover part of this checklist:

1. Managed-mode exclusive Wi-Fi ownership under Arduino core startup.
2. SoftAP provisioning using the Arduino socket HTTP adapter (`GET /health`, GET/POST `/set`).
3. Stored-profile boot and MQTT/TLS READY.
4. GPIO provisioning helper (10-second enter/cancel).
5. External Wi-Fi mode with Arduino `WiFi` + `configTime()`.
6. PLAIN publish/command RX.
7. AES interop when AirNgin backend AES validation is available.
8. AirNgin Time feed.
9. SMS / Push delivery.
10. Cloud Database HTTPS SET/GET/DELETE and TLS trust chain.
11. Unified Serial Test Console.

## First hardware check

Choose a registered test device and define the numeric `uptime_seconds` operation. Set its serial, producer and Wi-Fi setup password in DeviceUptime. Upload, provision, then compare the serial monitor with two consecutive cloud samples. Power-cycle once to confirm stored credentials and MQTT reconnection. With ManagedBasic, send `ch1=on` and `ch1=off` and verify their replies. Record the board, flash/partition and core version alongside the observations.

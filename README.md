# AirNgin ESP32 SDK — Arduino library 1.6.1

Arduino-ESP32 port of the AirNgin ESP-IDF SDK 1.6.0.

**Languages:** [فارسی](docs/fa/README.md) · [English](docs/en/README.md)

**Engineering notes:** [Porting report](PORTING_REPORT.md) · [Validation status](VALIDATION.md)

## What is included

- Managed Wi-Fi with AirNgin provisioning
- External-network mode (your sketch owns Wi-Fi/Ethernet/time)
- MQTT 3.1.1 over verified TLS
- PLAIN or optional AES-256-GCM application-layer mode
- Publish / Report APIs
- Server-to-device command callback
- AirNgin Time feed (UTC / Tehran)
- SMS / Push notification APIs
- Cloud Database over HTTPS (SET / GET / DELETE)
- Manual provisioning start/cancel
- 10-second GPIO provisioning-button helper
- Development Serial Test Console
- Diagnostic RX/TX logging

The low-level **C API from SDK 1.6.0 is preserved** in `airngin.h`. `AirNginESP32.h` adds an Arduino-friendly C++ facade; it does not replace the C API.

## Requirements

- Arduino-ESP32 **3.3.10** (Espressif Boards Manager)
- ESP32, ESP32-S2, ESP32-S3, ESP32-C3 or ESP32-C6 with integrated Wi-Fi and at least 4 MB flash
- A sufficiently large application partition. Full feature/debug builds can exceed 1 MB; use **Huge APP** or another partition with at least ~1.5 MB app space.

## Arduino IDE install

Library Manager search name after registration: **AirNginESP32SDK**. Use `#include <AirNginESP32SDK.h>` in new sketches. Existing sketches can keep `AirNginESP32.h`. To install the release ZIP directly:

1. Install Espressif **ESP32 by Espressif Systems** in Boards Manager.
2. `Sketch > Include Library > Add .ZIP Library...`
3. Select `AirNginESP32SDK-1.6.1.zip`.
4. Open `File > Examples > AirNginESP32SDK > ManagedBasic`.
5. Choose the matching ESP32 Dev Module and **Huge APP**. Set the registered serial, producer code and setup AP password in managed examples. For the provisioning button, wire a free input to ground through the button and set `provisionButtonPin` in the sketch.

The current AirNgin mobile app connects to the setup AP using `00000000`. Set `setupPassword` to that value for pairing with this app. A custom setup client can use a device-specific password.

For compile-time options, Arduino-ESP32 supports a `build_opt.h` file in the sketch folder. Examples that need AES/Database/verbose diagnostics already include one.

## First reading

Open **DeviceUptime** and configure the serial, producer code and setup password. Provision the board with the AirNgin app. Once MQTT is ready, the sketch publishes the board's uptime every 30 seconds. Register the numeric operation `uptime_seconds` (unit `s`) for your device to see it in the panel. The serial monitor reports whether each reading entered the SDK's queue. Confirm delivery in the panel.

**ManagedBasic** accepts `ch1=on` and `ch1=off`, updates a switch held in RAM and reports the resulting state. Both examples work without external wiring.

## Examples

| Example | Purpose |
|---|---|
| `DeviceUptime` | Publish uptime in seconds every 30 seconds; works without an external sensor |
| `ManagedBasic` | Smallest managed-mode integration |
| `ManagedProvisionButton` | the selected GPIO, 10-second provisioning toggle |
| `ExternalWiFi` | Arduino `WiFi` owned by the sketch |
| `AESExternal` | External mode + application-layer AES |
| `TimeFeed` | UTC/Tehran AirNgin time feed |
| `Notifications` | Center SMS, user SMS, user Push |
| `CloudDatabase` | HTTPS SET/GET/DELETE |
| `SerialTestConsole` | Unified developer console |

## Official AirNgin docs

- Main docs: https://docs.airngin.com/
- Time: https://docs.airngin.com/fa/docs/tutorial-time/getting-started
- Notification/SMS: https://docs.airngin.com/fa/docs/tutorial-sms-notfictaion-gateway/send-message-to-own/
- Cloud Database: https://docs.airngin.com/fa/docs/tutorial-database/getting-started

## Important security note

PLAIN mode means **no AirNgin application-layer AES**. MQTT is still protected with TLS and certificate/hostname verification. Never enable secret diagnostic logging in production.

## Validation status

See [the compile matrix and hardware acceptance steps](VALIDATION.md).

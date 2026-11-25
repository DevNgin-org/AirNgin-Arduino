# AirNgin ESP32 SDK — Arduino library 1.6.2

Arduino-ESP32 port of the AirNgin ESP-IDF SDK 1.6.0.

**Languages:** [فارسی](docs/fa/README.md) · [English](docs/en/README.md)

**Engineering notes:** [Porting report](PORTING_REPORT.md) · [Validation status](VALIDATION.md)

## Choose your board and SDK

| Setup guide | Supported build targets | Verification |
|---|---|---|
| [ESP32 with Arduino IDE or PlatformIO](docs/en/getting-started.md) | ESP32, S2, S3, C3 and C6 Dev Modules, Arduino-ESP32 3.3.10 | All ten examples, including DirectNetwork in its default Ethernet configuration, compiled for all five targets. On a physical S3, DeviceUptime reconnected to Wi-Fi after a restart, synchronized time and reached MQTT READY. |
| [ESP32-S3 with ESP-IDF](https://github.com/DevNgin-org/airngin-esp-idf-sdk/blob/main/docs/en/getting-started.md) | ESP32-S3, ESP-IDF 5.5.x | [Build and host-test results](https://github.com/DevNgin-org/airngin-esp-idf-sdk/blob/main/VALIDATION.md) cover specific configurations. See the report for the hardware checks still open. |
| [STM32 with Ethernet](https://github.com/DevNgin-org/AirNginSTM32LAN#readme) | NUCLEO-F401RE with a W5100 shield | The LanUptime firmware compiled for the F401RE. Discovery from a phone and cloud delivery on the physical board are pending. |

The additional [DirectNetwork example](examples/DirectNetwork/DirectNetwork.ino) compiled for ESP32-S3 in both Ethernet and manual Wi-Fi modes. On-board pairing with the app remains to be verified. For the scope of the S3 bench test, see the [Arduino validation report](VALIDATION.md).

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
3. Select `AirNginESP32SDK-1.6.2.zip`.
4. Open `File > Examples > AirNginESP32SDK > ManagedBasic`.
5. Choose the matching ESP32 Dev Module and **Huge APP**. Set the registered serial, producer code and setup AP password in managed examples. For the provisioning button, wire a free input to ground through the button and set `provisionButtonPin` in the sketch.

The current AirNgin mobile app connects to the setup AP using `00000000`. Set `setupPassword` to that value for pairing with this app. A custom setup client can use a device-specific password.

For compile-time options, Arduino-ESP32 supports a `build_opt.h` file in the sketch folder. Examples that need AES/Database/verbose diagnostics already include one.

## First reading

Open **DeviceUptime** and configure the serial, producer code and setup password. Provision the board with the AirNgin app. Once MQTT is ready, the sketch publishes the board's uptime every 30 seconds. Register the numeric operation `uptime_seconds` (unit `s`) for your device to see it in the panel. The serial monitor reports whether each reading entered the SDK's queue. Confirm delivery in the panel.

**ManagedBasic** accepts `ch1=on` and `ch1=off`, updates a switch held in RAM and reports the resulting state. Both examples work without external wiring.

## Examples

| Example | When to use it |
|---|---|
| [ManagedBasic](examples/ManagedBasic/ManagedBasic.ino) | Pair through the board’s temporary Wi-Fi (SoftAP), then switch a virtual channel on and off with the ch1 command. |
| [DirectNetwork](examples/DirectNetwork/DirectNetwork.ino) | Pair from the mobile app on the same LAN. Select Ethernet or manually configured Wi-Fi. After the board gets an IP address, the app discovers it via mDNS. |
| [ExternalWiFi](examples/ExternalWiFi/ExternalWiFi.ino) | Enter Wi-Fi and MQTT credentials in the sketch. Use this when your own firmware manages the connection. |
| [DeviceUptime](examples/DeviceUptime/DeviceUptime.ino) | Publish uptime in seconds every 30 seconds to test telemetry without a sensor. |
| [ManagedProvisionButton](examples/ManagedProvisionButton/ManagedProvisionButton.ino) | Hold a physical button for 10 seconds to open setup. Hold it again for 10 seconds to cancel. |
| [AESExternal](examples/AESExternal/AESExternal.ino) | Use manually supplied credentials and an AES-256-GCM key to encrypt application messages over MQTT/TLS. |
| [TimeFeed](examples/TimeFeed/TimeFeed.ino) | Subscribe to the AirNgin Tehran time feed and print the received time in Serial Monitor. |
| [Notifications](examples/Notifications/Notifications.ino) | Trigger an SMS to the center or owner, or a push notification to the owner, using an approved message code. |
| [CloudDatabase](examples/CloudDatabase/CloudDatabase.ino) | Use Serial Monitor to write, read or delete a test key in the AirNgin cloud database over HTTPS. |
| [SerialTestConsole](examples/SerialTestConsole/SerialTestConsole.ino) | Use the serial console to test SDK commands and inspect device events during development. |

In **DirectNetwork**, set `deviceSerial` and `producer` to the registered device identifiers. Leave `connection` at `Connection::Ethernet` for wired LAN, or select `Connection::ManualWiFi` and fill in `wifiSSID` and `wifiPassword`. On first setup, the app has three minutes after the board gets an IP address to send the project settings. Type `p` in Serial Monitor to reopen setup.

## Official AirNgin docs

- Main docs: https://docs.airngin.com/
- Time: https://docs.airngin.com/fa/docs/tutorial-time/getting-started
- Notification/SMS: https://docs.airngin.com/fa/docs/tutorial-sms-notfictaion-gateway/send-message-to-own/
- Cloud Database: https://docs.airngin.com/fa/docs/tutorial-database/getting-started

## Important security note

PLAIN mode means **no AirNgin application-layer AES**. MQTT is still protected with TLS and certificate/hostname verification. Never enable secret diagnostic logging in production.

## Validation status

See [the compile matrix and hardware acceptance steps](VALIDATION.md).

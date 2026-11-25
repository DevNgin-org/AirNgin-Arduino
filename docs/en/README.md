# AirNgin ESP32 SDK Arduino Documentation — 1.6.1

This is the Arduino-ESP32 port of the current AirNgin SDK 1.6.0. The port keeps the proven Cloud/MQTT/provisioning contracts while making setup practical in Arduino IDE and PlatformIO.

## Start here

1. [Installation and build configuration](installation.md)
2. [Quick start](getting-started.md)
3. Choose a network ownership model:
   - [Managed Wi-Fi](managed-wifi.md)
   - [External Network](external-network.md)
4. Features:
   - [Provisioning](provisioning.md)
   - [Publish / Report and commands](data-and-commands.md)
   - [Time, SMS/Push and Cloud Database](cloud-features.md)
   - [TLS, AES and build options](security-and-config.md)
5. For hardware testing: [Serial Test Console](serial-console.md)
6. [API reference](api-reference.md)
7. Problems: [Troubleshooting](troubleshooting.md)

> The low-level C API in `airngin.h` is the 1.6.0 API. The global `AirNgin` C++ object is a convenience layer for Arduino sketches.

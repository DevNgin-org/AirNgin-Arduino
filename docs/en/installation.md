# Installation and build configuration

## Arduino IDE

Once indexed, search for **AirNginESP32SDK** in Library Manager. Include `AirNginESP32SDK.h` in new sketches. The earlier `AirNginESP32.h` header remains supported. For provisioning with the current AirNgin mobile app, use `00000000` for the device setup AP password in the sketch.

1. Install **ESP32 by Espressif Systems** 3.3.10 from Boards Manager.
2. Use `Sketch > Include Library > Add .ZIP Library` and select the AirNgin ESP32 SDK ZIP.
3. Select the Dev Module matching your chip: ESP32, S2, S3, C3 or C6.
4. For full AES/Database/diagnostic builds select a large partition such as **Huge APP**. Full images may exceed 1 MB.

## Compile-time options in Arduino IDE

Arduino-ESP32 applies a sketch-local `build_opt.h` to all compilation units. Example:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
-DCONFIG_AIRNGIN_DIAGNOSTIC_VERBOSE=1
-DCONFIG_AIRNGIN_USE_AES=0
```

See `extras/build_opt.example.h`. Feature examples such as `CloudDatabase` and `AESExternal` already include their required `build_opt.h`.

## PlatformIO

This configuration is optional. Release builds use Arduino CLI.

See `extras/platformio.ini.example`:

```ini
[env:esp32-s3-devkitc-1]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
board_build.partitions = huge_app.csv

build_flags =
    -DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
    -DCONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1
```

## Key defaults

| Option | Default | Purpose |
|---|---:|---|
| `CONFIG_AIRNGIN_USE_AES` | 0 | Application-layer AES off |
| `CONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE` | 0 | Optional HTTPS database support |
| `CONFIG_AIRNGIN_ENABLE_MANAGED_WIFI` | 1 | Managed Wi-Fi compiled in |
| `CONFIG_AIRNGIN_DIAGNOSTIC_LOGS` | 0 | Diagnostic logging |
| `CONFIG_AIRNGIN_DIAGNOSTIC_LOG_SECRETS` | 0 | **Never enable in production** |
| `CONFIG_AIRNGIN_TASK_STACK_SIZE` | 16384 | AirNgin owner-task stack |
| `CONFIG_AIRNGIN_EXTERNAL_RECONNECT_DELAY_MS` | 3000 | External reconnect delay |

See `src/AirNginConfig.h` for the complete list.

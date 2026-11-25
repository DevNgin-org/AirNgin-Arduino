# Troubleshooting

## Managed mode returns `ESP_ERR_INVALID_STATE`

Your sketch probably initialized the Wi-Fi driver/default netifs before AirNgin. Do not call `WiFi.begin()` in Managed mode. Use External mode when the application must own networking.

## Device opens provisioning

Common reasons:

- no valid profile;
- profile marked for credential recovery;
- explicit API/button provisioning request.

## MQTT will not connect

Check:

- system clock is trusted;
- current AirNgin username/password/client ID;
- outbound TLS/8883 is reachable;
- enable diagnostics and inspect the real CONNACK result.

## Image does not fit a 1 MB partition

Cloud Database, AES and diagnostics add code. Select a larger partition such as Huge APP.

## Database returns `ESP_ERR_NOT_SUPPORTED`

Enable it globally in sketch `build_opt.h`:

```text
-DCONFIG_AIRNGIN_ENABLE_CLOUD_DATABASE=1
```

In AES builds Database is blocked by default because that HTTPS API has no AirNgin application-layer AES form. Only enable `CONFIG_AIRNGIN_DATABASE_ALLOW_IN_AES_BUILD=1` after an explicit security decision.

## Serial console does not accept commands

- 115200 baud
- `LF` line ending
- call `AirNginConsole.loop()` frequently from Arduino `loop()`.

## `DOWNLOAD(USB/UART0) waiting for download`

The ESP32 is in ROM download/bootloader mode; it is not downloading from the Internet. Release BOOT/GPIO0 and reset the board.

## Provisioning URI length

The Arduino port uses its own socket-based provisioning HTTP transport, so compatible long `GET /set?data=...` requests are not tied to the prebuilt core's `CONFIG_HTTPD_MAX_URI_LEN`.

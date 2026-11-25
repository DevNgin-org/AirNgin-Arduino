# Sending data and receiving commands

## Publish and Report

```cpp
AirNgin.publish("ch1", "on");
AirNgin.report("ch1", "on");
```

These APIs do not expose raw MQTT. The SDK builds the correct AirNgin topic/payload. The same calls are used in AES builds; encryption is internal.

## Receive a command

The server may deliver an envelope such as:

```json
{"data":"{\"key\":\"ch1\",\"value\":\"on\"}","deviceSerial":"VTRA0000000000"}
```

Your command handler receives the logical values:

```cpp
static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *) {
  digitalWrite(15, !strcmp(cmd->value, "on") ? HIGH : LOW);
  return airngin_command_result_set(result, cmd->value);
}
```

With `CONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1`, raw MQTT RX and the parsed command are both visible on Serial.

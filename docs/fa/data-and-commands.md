# ارسال داده و دریافت فرمان

## Publish و Report

```cpp
AirNgin.publish("ch1", "on");
AirNgin.report("ch1", "on");
```

این توابع به MQTT خام دسترسی نمی‌دهند؛ SDK topic و JSON صحیح AirNgin را می‌سازد. در AES build نیز همان API استفاده می‌شود و encryption داخل SDK انجام می‌شود.

## دریافت فرمان

سرور ممکن است payloadی شبیه این بفرستد:

```json
{"data":"{\"key\":\"ch1\",\"value\":\"on\"}","deviceSerial":"VTRA0000000000"}
```

Application callback فقط داده منطقی را می‌بیند:

```cpp
static esp_err_t onCommand(const airngin_command_t *cmd,
                           airngin_command_result_t *result,
                           void *) {
  // cmd->key   == "ch1"
  // cmd->value == "on"
  digitalWrite(15, !strcmp(cmd->value, "on") ? HIGH : LOW);
  return airngin_command_result_set(result, cmd->value);
}
```

با `CONFIG_AIRNGIN_DIAGNOSTIC_LOGS=1`، پیام MQTT خام و نتیجه Parse هم در Serial دیده می‌شود.

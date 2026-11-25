# Unified Serial Test Console

A single development console can run on any Arduino `Stream`.

```cpp
#include <AirNginTestConsole.h>

static void onEvent(const airngin_event_data_t *event, void *) {
  AirNginConsole.handleEvent(event);
}

void setup() {
  Serial.begin(115200);
  AirNgin.beginManaged("VTRA0000000000", "VTRA", nullptr, onEvent);
  AirNginConsole.begin(Serial);
}

void loop() {
  AirNginConsole.loop();
}
```

Serial Monitor: `115200`, line ending `LF`.

## Commands

```text
help
status
publish <key> <value>
report <key> <value>
db set <key> <value>
db get <key>
db delete <key>
sms center <code>
sms user <code>
push user <code>
time utc
time tehran
time show
time off
mqtt tls status
prov status
prov start
prov cancel
```

Values can contain spaces:

```text
db set greeting "hello world"
publish message "door opened"
```

This console is a development/hardware-validation tool, not a product UI. SMS/Push may incur cost and are sent only after an explicit command.

See `examples/SerialTestConsole`.

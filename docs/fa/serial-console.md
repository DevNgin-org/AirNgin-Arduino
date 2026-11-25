# Unified Serial Test Console

برای تست آزمایشگاهی می‌توانید یک کنسول واحد روی هر `Stream` Arduino اجرا کنید.

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

تنظیم Serial Monitor: `115200` و Line ending روی `LF`.

## Commandها

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

Value دارای فاصله هم پشتیبانی می‌شود:

```text
db set greeting "hello world"
publish message "door opened"
```

این Console ابزار Development است، UI محصول نیست. SMS/Push ممکن است هزینه داشته باشد و فقط با command صریح ارسال می‌شود.

Example کامل: `examples/SerialTestConsole`.

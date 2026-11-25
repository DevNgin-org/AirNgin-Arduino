# Provisioning

Provisioning is available in Managed Wi-Fi mode only.

Without a valid profile AirNgin opens an AP named roughly:

```text
<deviceSerial>_<provisioningLabel>
```

The AP uses `192.168.1.1` and exposes:

- `GET /health`
- `GET /set?data=...` or `POST /set`

The Arduino port uses a small socket-based provisioning HTTP server so long compatible GET requests are not constrained by the prebuilt Arduino core's fixed HTTP URI buffer.

## Manual API

```cpp
AirNgin.requestProvisioning();
AirNgin.cancelProvisioning();
```

## 10-second button helper

```cpp
#include <AirNginProvisionButton.h>
AirNginProvisionButton button;
constexpr int buttonPin = -1; // set to a free GPIO before wiring the button

void setup() { button.begin(buttonPin, 10000, 50); }
void loop() { button.update(); }
```

Behavior:

- Hold the selected GPIO LOW for 10 seconds → enter provisioning
- Release → panel remains open
- Hold again for 10 seconds → cancel
- Default panel timeout → 120 seconds
- Successful provisioning → save/readback and restart

The button helper is an Arduino utility, not part of the AirNgin wire protocol.

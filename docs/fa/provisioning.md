# Provisioning

Provisioning فقط در Managed Wi-Fi وجود دارد.

وقتی Profile معتبر نداریم، AirNgin یک AP با نامی شبیه زیر می‌سازد:

```text
<deviceSerial>_<provisioningLabel>
```

AP روی `192.168.1.1` است و دو endpoint دارد:

- `GET /health`
- `GET /set?data=...` یا `POST /set`

نسخه Arduino برای حفظ سازگاری با payloadهای طولانی، HTTP Provisioning را با یک socket server کوچک پیاده می‌کند و به محدودیت ثابت URI در Core استاندارد وابسته نیست.

## API دستی

```cpp
AirNgin.requestProvisioning();
AirNgin.cancelProvisioning();
```

## Helper دکمه 10 ثانیه‌ای

```cpp
#include <AirNginProvisionButton.h>
AirNginProvisionButton button;
constexpr int buttonPin = -1; // set to a free GPIO before wiring the button

void setup() {
  button.begin(buttonPin, 10000, 50);
}

void loop() {
  button.update();
}
```

رفتار:

- پایهٔ انتخابی برای 10 ثانیه LOW → ورود به Provisioning
- رها کردن دکمه → پنل باز می‌ماند
- نگه‌داشتن دوباره برای 10 ثانیه → Cancel
- Timeout پیش‌فرض پنل → 120 ثانیه
- Provisioning موفق → Save/Readback و Restart

Helper دکمه جزو API عمومی Cloud نیست؛ فقط utility نسخه Arduino است.

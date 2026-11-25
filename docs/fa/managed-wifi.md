# Managed Wi-Fi

در این حالت AirNgin مالک شبکه Wi-Fi است. Sketch نباید قبل از `AirNgin.beginManaged()` از `WiFi.begin()` یا API دیگری که Wi-Fi driver/default STA/AP netif را ایجاد می‌کند استفاده کند.

AirNgin مدیریت می‌کند:

- Wi-Fi driver
- `WIFI_STA_DEF` و `WIFI_AP_DEF`
- reconnect
- SNTP برای معتبر شدن ساعت TLS
- SoftAP و HTTP Provisioning
- MQTT/TLS

NVS، `esp_netif` core و default event loop منابع shared هستند و AirNgin آنها را پاک یا delete نمی‌کند.

```cpp
AirNgin.beginManaged(
  "VTRA0000000000",
  "VTRA",
  onCommand,
  onEvent,
  nullptr,
  "Device",
  setupPassword
);
```

اگر برنامه شما باید خودش Wi-Fi/Ethernet را مدیریت کند، از [External Network](external-network.md) استفاده کنید.

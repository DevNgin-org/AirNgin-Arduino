# Managed Wi-Fi

In this mode AirNgin owns Wi-Fi. Your sketch must not call `WiFi.begin()` or otherwise create the Wi-Fi driver/default STA/AP netifs before `AirNgin.beginManaged()`.

AirNgin owns:

- Wi-Fi driver
- `WIFI_STA_DEF` / `WIFI_AP_DEF`
- reconnects
- SNTP used to establish a trusted TLS clock
- provisioning SoftAP and HTTP endpoint
- MQTT/TLS

NVS, esp-netif core and the default event loop are treated as shared process infrastructure and are not erased/deleted by AirNgin.

```cpp
AirNgin.beginManaged("VTRA0000000000", "VTRA", onCommand, onEvent,
                     nullptr, "Device", setupPassword);
```

Use [External Network](external-network.md) when your sketch must own Wi-Fi/Ethernet/time.

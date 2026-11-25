// Already on a network? Choose Ethernet or ManualWiFi below. For SoftAP pairing,
// open the existing ManagedBasic example. This sketch owns the network interface.
#include <AirNginESP32SDK.h>
#include <ETH.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <mdns.h>
#include <memory>
#include <new>
#include "DirectProfile.h"

enum class Connection { Ethernet, ManualWiFi };
constexpr Connection connection = Connection::Ethernet;
constexpr char deviceSerial[] = "VTRA0000000000";  // Set both registered identifiers.
constexpr char producer[] = "VTRA";
constexpr char wifiSSID[] = "";                   // Used only in ManualWiFi mode.
constexpr char wifiPassword[] = "";

Preferences store;
std::unique_ptr<WebServer> http;
IPAddress currentIP;
bool initialized, firstPairing, pairing, advertised, pending, sdkRunning, networkUp;
uint32_t openedAt, lastAdvertise, lastApply, lastReport;

bool startLink() {
    if (connection == Connection::ManualWiFi) {
        if (!wifiSSID[0] || !WiFi.mode(WIFI_STA)) return false;
        WiFi.setAutoReconnect(true);
        WiFi.begin(wifiSSID, wifiPassword);
        return true;
    }
#if defined(ETH_PHY_TYPE)
    return ETH.begin();       // Board-defined Ethernet PHY and pinout.
#elif CONFIG_ETH_SPI_ETHERNET_W5500 && ETH_SPI_SUPPORTS_CUSTOM
    SPI.begin();              // W5500 on default SPI pins and SS; customize for your board.
    return ETH.begin(ETH_PHY_W5500, 1, SS, -1, -1, SPI);
#else
    return false;
#endif
}

void openPairing() {
    openedAt = millis();
    pairing = true;
    Serial.println("Local setup open for 3 minutes");
}

void receiveProfile() {
    if (!pairing || millis()-openedAt >= 180000) { http->send(403); return; }
    if (http->args()!=1 || http->argName(0)!="data") { http->send(400); return; }
    const String data = http->arg(0);
    DirectProfile profile{};
    if (!parseDirectProfile(data.c_str(), data.length(), &profile)) { http->send(400); return; }
    if (store.putBytes(deviceSerial,&profile,sizeof profile)!=sizeof profile) { http->send(500); return; }
    pairing = false;
    pending = true;
    lastApply = millis()-5000;
    http->sendHeader("Cache-Control","no-store");
    http->send(200,"application/json","{\"ok\":true}");
}

esp_err_t applyStored() {
    DirectProfile p{};
    if (store.getBytesLength(deviceSerial)!=sizeof p || store.getBytes(deviceSerial,&p,sizeof p)!=sizeof p)
        return ESP_ERR_INVALID_SIZE;
    const auto cloud = directCredentials(&p);
    // The SDK validates bounded fields and copies credentials before returning.
    esp_err_t err = sdkRunning ? AirNgin.replaceExternalCredentials(cloud) :
                                 AirNgin.beginExternal(deviceSerial,producer,cloud);
    if (err==ESP_OK) sdkRunning=true;
    return err;
}

void setup() {
    Serial.begin(115200);
    if (strlen(deviceSerial)!=14 || strlen(producer)!=4 || strncmp(deviceSerial,producer,4) ||
        strspn(deviceSerial+4,"0123456789")!=10) {
        Serial.println("Check the registered serial and producer code");
        return;
    }
    if (!startLink() || !store.begin(CONFIG_AIRNGIN_USE_AES ? "ag-lan-aes-ex1" : "ag-lan-ex1",false)) {
        Serial.println("Check network settings and storage");
        return;
    }
    pending = store.isKey(deviceSerial);
    firstPairing = !pending;
    initialized = true;
    lastApply = millis()-5000;
}

void loop() {
    delay(5);
    if (!initialized) return;
    IPAddress ip;
    if (connection==Connection::ManualWiFi && WiFi.status()==WL_CONNECTED) ip=WiFi.localIP();
    if (connection==Connection::Ethernet && ETH.linkUp()) ip=ETH.localIP();
    if (ip!=currentIP) {
        http.reset();
        if (advertised) MDNS.end();
        advertised=false;
        if (networkUp && AirNgin.setExternalNetworkConnected(false)==ESP_OK) networkUp=false;
        currentIP=ip;
        if (ip) {
            configTime(0,0,"pool.ntp.org","time.cloudflare.com");
            lastAdvertise=millis()-3000;
            if (firstPairing) { openPairing(); firstPairing=false; }
        }
    }
    if (!ip) return;
    if (!advertised && millis()-lastAdvertise>=3000) {
        lastAdvertise=millis();
        if (MDNS.begin(deviceSerial)) {
            mdns_txt_item_t sn={"sn",deviceSerial};
            advertised=mdns_service_add(deviceSerial,"_matterc","_udp",45454,&sn,1)==ESP_OK;
            if (!advertised) MDNS.end();
        }
    }
    // A local operator can reopen the setup window by typing p in Serial Monitor.
    if (Serial.available() && Serial.read()=='p') openPairing();
    if (pairing && millis()-openedAt>=180000) pairing=false;
    if (pairing && !http) {
        http.reset(new(std::nothrow) WebServer(ip,80));
        if (http) { http->on("/set",HTTP_GET,receiveProfile); http->begin(); }
    }
    if (pairing && http) http->handleClient();
    if (!pairing) http.reset();
    if (pending && millis()-lastApply>=5000) {
        lastApply=millis();
        esp_err_t err=applyStored();
        if (err==ESP_OK) pending=false;
        if (err==ESP_ERR_INVALID_ARG || err==ESP_ERR_INVALID_SIZE) { pending=false; openPairing(); }
    }
    if (sdkRunning && !networkUp && AirNgin.setExternalNetworkConnected(true)==ESP_OK) networkUp=true;
    if (AirNgin.ready() && millis()-lastReport>=30000) {
        lastReport=millis();
        char seconds[16];
        snprintf(seconds,sizeof seconds,"%lu",(unsigned long)(lastReport/1000));
        AirNgin.publish("uptime_seconds",seconds);
    }
}

#include <Arduino.h>
#include <AirNginESP32.h>
#include <AirNginProvisionButton.h>
#include <AirNginTestConsole.h>

AirNginProvisionButton button;

void setup() {
    Serial.begin(115200);
    // Compile smoke test only. No cloud connection is started automatically.
    Serial.println("AirNginESP32 compile smoke image");
}

void loop() {
    delay(1000);
}

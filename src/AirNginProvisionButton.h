#pragma once
#include <Arduino.h>
#include "AirNginESP32.h"

/** Arduino sample helper for the 10-second active-low provisioning toggle. */
class AirNginProvisionButton {
public:
    void begin(int pin = 16, uint32_t holdMs = 10000, uint32_t debounceMs = 50);
    void update();
    bool enabled() const { return _pin >= 0; }

private:
    enum State : uint8_t { IDLE, DEBOUNCE_LOW, HOLDING, TRIGGERED, DEBOUNCE_HIGH };
    int _pin = -1;
    uint32_t _holdMs = 10000;
    uint32_t _debounceMs = 50;
    State _state = IDLE;
    State _beforeHigh = IDLE;
    int64_t _edgeUs = 0;
    int64_t _holdStartUs = 0;
};

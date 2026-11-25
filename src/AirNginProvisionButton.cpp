#include "AirNginProvisionButton.h"
#include "esp_timer.h"

void AirNginProvisionButton::begin(int pin, uint32_t holdMs, uint32_t debounceMs)
{
    _pin = pin;
    _holdMs = holdMs;
    _debounceMs = debounceMs;
    _state = IDLE;
    if (_pin >= 0) pinMode(_pin, INPUT_PULLUP);
}

void AirNginProvisionButton::update()
{
    if (_pin < 0) return;
    const bool low = digitalRead(_pin) == LOW;
    const int64_t now = esp_timer_get_time();
    const int64_t debounceUs = (int64_t)_debounceMs * 1000LL;
    const int64_t holdUs = (int64_t)_holdMs * 1000LL;

    switch (_state) {
    case IDLE:
        if (low) { _edgeUs = now; _state = DEBOUNCE_LOW; }
        break;
    case DEBOUNCE_LOW:
        if (!low) _state = IDLE;
        else if (now - _edgeUs >= debounceUs) { _holdStartUs = now; _state = HOLDING; }
        break;
    case HOLDING:
        if (!low) { _edgeUs = now; _beforeHigh = HOLDING; _state = DEBOUNCE_HIGH; }
        else if (now - _holdStartUs >= holdUs) {
            airngin_status_t st = {};
            st.struct_size = sizeof(st);
            if (airngin_get_status(&st) == ESP_OK && st.lifecycle == AIRNGIN_STATE_RUNNING) {
                if (st.provisioning_active) (void)airngin_cancel_provisioning();
                else (void)airngin_request_provisioning();
                _state = TRIGGERED;
            }
        }
        break;
    case TRIGGERED:
        if (!low) { _edgeUs = now; _beforeHigh = TRIGGERED; _state = DEBOUNCE_HIGH; }
        break;
    case DEBOUNCE_HIGH:
        if (low) {
            /* A HIGH shorter than debounce is release bounce; resume the previous state. */
            _state = _beforeHigh;
        } else if (now - _edgeUs >= debounceUs) {
            _state = IDLE;
        }
        break;
    }
}

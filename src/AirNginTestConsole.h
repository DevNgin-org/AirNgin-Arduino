#pragma once
#include <Arduino.h>
#include "AirNginESP32.h"
#include "freertos/queue.h"

/** Development-only interactive Serial/Stream test console. */
class AirNginTestConsoleClass {
public:
    esp_err_t begin(Stream &io = Serial);
    void loop();
    void handleEvent(const airngin_event_data_t *event);
    bool running() const { return _io != nullptr; }

private:
    Stream *_io = nullptr;
    QueueHandle_t _eventQueue = nullptr;
    char *_line = nullptr;
    size_t _used = 0;
    bool _discard = false;
    bool _promptPrinted = false;

    void prompt();
    void help();
    void execute(char *line);
    bool drainEvents();
};

extern AirNginTestConsoleClass AirNginConsole;

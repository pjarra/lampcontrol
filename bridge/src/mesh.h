// Bluetooth side of the bridge (ESP32 only): one mesh connection for commands and status, and
// short direct connections to each lamp to set its clock (clock frames carry no lamp address).
#pragma once
#include "model.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string>

namespace mesh {
// model is shared with the web server; every access goes through lock.
void start(Model* model, SemaphoreHandle_t lock, const std::string& name, const std::string& password, const std::string& posixTZ);
std::string connectedLamp();   // "" when not connected
std::string message();         // short status for the pages
std::string info();            // diagnostics lines for /api/info
void requestClocks();          // set every lamp's clock as soon as the time is known
bool timeKnown();
}

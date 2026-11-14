#pragma once

#include "esp_err.h"

// Start Wi-Fi, automatic reconnect, and periodic publication of sensor readings.
esp_err_t home_station_mqtt_start(void);

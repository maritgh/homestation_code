#pragma once

#include "esp_err.h"

// Start Wi-Fi, automatisch opnieuw verbinden en het periodiek publiceren van sensormetingen.
esp_err_t home_station_mqtt_start(void);

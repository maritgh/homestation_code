#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    bool distance_valid;
    bool climate_valid;
    bool water_temperature_valid;
    uint16_t distance_mm;
    float temperature_c;
    float humidity_percent;
    float water_temperature_c;
} sensor_readings_t;

esp_err_t sensors_start(i2c_master_bus_handle_t bus);
// Kopieer de laatste meetgegevens zonder ze te verwijderen; scherm en MQTT kunnen ze beide lezen.
bool sensors_get_latest(sensor_readings_t *readings);

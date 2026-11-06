#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid;
    int64_t sampled_at_us;
    float speed_ms;
    float direction_deg; // 0 = north, 90 = east, 180 = south, 270 = west.
} wind_readings_t;

// Start independent HX711 sampling and automatic startup tare.
esp_err_t wind_start(void);
// Copy the latest snapshot without consuming it. Check valid before publishing.
bool wind_get_latest(wind_readings_t *readings);

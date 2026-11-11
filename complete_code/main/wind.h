#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool valid; // Minstens één gereedstaande sensor met nulstelling levert een eindige snelheid.
    int64_t sampled_at_us;
    float speed_ms; // Gemiddelde van beschikbare gelijkgerichte sensoren, inclusief A/B-kalibratie.
} wind_readings_t;

// Start onafhankelijke HX711-metingen met automatische nulstelling bij het opstarten.
esp_err_t wind_start(void);
// Kopieer de laatste meetgegevens zonder ze te verwijderen. Controleer valid vóór publicatie.
bool wind_get_latest(wind_readings_t *readings);

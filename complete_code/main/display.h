#pragma once

#include "driver/i2c_master.h"

// Start het scherm één keer, voordat sensors_start() de gedeelde bus gaat gebruiken.
void display_start(i2c_master_bus_handle_t bus);

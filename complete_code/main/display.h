#pragma once

#include "driver/i2c_master.h"

// Start the display once, before sensors_start() begins using the shared bus.
void display_start(i2c_master_bus_handle_t bus);

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

esp_err_t vl53l4cd_init(i2c_master_dev_handle_t dev);
esp_err_t vl53l4cd_read(i2c_master_dev_handle_t dev, uint16_t *mm, bool *valid);

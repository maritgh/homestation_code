#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64
#define SH1106_PAGE_COUNT (SH1106_HEIGHT / 8)
#define SH1106_ALL_PAGES ((1U << SH1106_PAGE_COUNT) - 1U)
#define SH1106_FRAME_BYTES (SH1106_WIDTH * SH1106_PAGE_COUNT)

// Initialize and clear the panel before starting other users of the I2C bus.
esp_lcd_panel_io_handle_t sh1106_init(i2c_master_bus_handle_t bus);

// Send a complete frame of eight vertical-bit pages. The caller locks the bus.
esp_err_t sh1106_draw(esp_lcd_panel_io_handle_t io, const uint8_t *pixels);

// Send selected pages (bit 0 selects the first page). The caller locks the bus.
esp_err_t sh1106_draw_pages(esp_lcd_panel_io_handle_t io, const uint8_t *pixels, uint8_t pages);

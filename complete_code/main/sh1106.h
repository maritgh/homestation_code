#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"

#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64
#define SH1106_PAGE_COUNT (SH1106_HEIGHT / 8)
#define SH1106_ALL_PAGES ((1U << SH1106_PAGE_COUNT) - 1U)
#define SH1106_FRAME_BYTES (SH1106_WIDTH * SH1106_PAGE_COUNT)

// Initialiseer en wis het scherm voordat andere gebruikers van de I2C-bus starten.
esp_lcd_panel_io_handle_t sh1106_init(i2c_master_bus_handle_t bus);

// Verstuur een volledig frame van acht pagina's met verticale bits. De aanroeper vergrendelt de bus.
esp_err_t sh1106_draw(esp_lcd_panel_io_handle_t io, const uint8_t *pixels);

// Verstuur geselecteerde pagina's (bit 0 kiest de eerste). De aanroeper vergrendelt de bus.
esp_err_t sh1106_draw_pages(esp_lcd_panel_io_handle_t io, const uint8_t *pixels, uint8_t pages);

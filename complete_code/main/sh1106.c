#include "sh1106.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SH1106_I2C_ADDRESS 0x3C
#define SH1106_I2C_CLOCK_HZ 100000
#define SH1106_COLUMN_OFFSET 2

static void send_init_commands(esp_lcd_panel_io_handle_t io)
{
    // Commandoreeks: scherm uit, klok, multiplex, offset, startregel,
    // DC/DC aan, segment-/COM-richting, COM-pinnen, contrast, voorladen,
    // VCOM, RAM-weergavemodus en normale (niet-omgekeerde) weergave.
    static const uint8_t commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0xAD, 0x8B, 0xA1,
        0xC8, 0xDA, 0x12, 0x81, 0x80, 0xD9, 0x22, 0xDB, 0x35, 0xA4, 0xA6,
    };
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, -1, commands, sizeof(commands)));
}

esp_err_t sh1106_draw(esp_lcd_panel_io_handle_t io, const uint8_t *pixels)
{
    return sh1106_draw_pages(io, pixels, SH1106_ALL_PAGES);
}

esp_err_t sh1106_draw_pages(esp_lcd_panel_io_handle_t io, const uint8_t *pixels, uint8_t pages)
{
    for (int page = 0; page < SH1106_PAGE_COUNT; ++page) {
        if ((pages & (1U << page)) == 0) {
            continue;
        }
        const uint8_t address[] = {
            0xB0 | page,
            SH1106_COLUMN_OFFSET & 0x0F,
            0x10 | (SH1106_COLUMN_OFFSET >> 4),
        };
        esp_err_t err = esp_lcd_panel_io_tx_param(io, -1, address, sizeof(address));
        if (err != ESP_OK) {
            return err;
        }
        err = esp_lcd_panel_io_tx_color(io, -1, pixels + page * SH1106_WIDTH, SH1106_WIDTH);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_lcd_panel_io_handle_t init_panel_io(i2c_master_bus_handle_t bus)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = SH1106_I2C_ADDRESS,
        .scl_speed_hz = SH1106_I2C_CLOCK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_config, &io_handle));
    return io_handle;
}

esp_lcd_panel_io_handle_t sh1106_init(i2c_master_bus_handle_t bus)
{
    esp_lcd_panel_io_handle_t io_handle = init_panel_io(bus);
    send_init_commands(io_handle);
    static const uint8_t blank[SH1106_FRAME_BYTES] = {0};
    ESP_ERROR_CHECK(sh1106_draw(io_handle, blank));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xAF, NULL, 0));
    vTaskDelay(pdMS_TO_TICKS(100));
    return io_handle;
}

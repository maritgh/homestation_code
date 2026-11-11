#include "display.h"

#include <assert.h>
#include <unistd.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "display_ui.h"
#include "i2c_shared.h"
#include "sh1106.h"

static const char *TAG = "display";

#define LVGL_TASK_STACK_SIZE (4 * 1024)
#define LVGL_TASK_PRIORITY 2
#define LVGL_PALETTE_SIZE 8
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)

// De SH1106 slaat pixels op in verticale pagina's. LVGL levert horizontale rijen.
static uint8_t display_buffer[SH1106_FRAME_BYTES];
static bool display_needs_full_refresh = true;

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_io_handle_t io = lv_display_get_user_data(disp);

    // Sla de twee paletwaarden van 4 bytes over in de monochrome LVGL-buffer.
    px_map += LVGL_PALETTE_SIZE;

    uint8_t dirty_pages = display_needs_full_refresh ? SH1106_ALL_PAGES : 0;
    const uint16_t width = lv_display_get_physical_horizontal_resolution(disp);
    for (int y = area->y1; y <= area->y2; ++y) {
        for (int x = area->x1; x <= area->x2; ++x) {
            // LVGL begint elke rij met de MSB het OLED gebruikt omgekeerde verticale bits.
            const uint8_t source_mask = 1U << (7 - x % 8);
            const bool pixel_set = (px_map[(width / 8) * y + x / 8] & source_mask) != 0;
            const uint8_t target_mask = 1U << (y % 8);
            uint8_t *pixel_byte = &display_buffer[width * (y / 8) + x];
            const uint8_t updated =
                pixel_set ? (*pixel_byte & (uint8_t)~target_mask) : (*pixel_byte | target_mask);
            if (updated != *pixel_byte) {
                *pixel_byte = updated;
                dirty_pages |= 1U << (y / 8);
            }
        }
    }
    if (dirty_pages != 0) {
        shared_i2c_acquire();
        const esp_err_t draw_err = sh1106_draw_pages(io, display_buffer, dirty_pages);
        shared_i2c_release();
        // Herhaal een gedeeltelijke of mislukte overdracht, ook als de pixels niet veranderen.
        display_needs_full_refresh = draw_err != ESP_OK;
        if (draw_err != ESP_OK) {
            ESP_LOGW(TAG, "OLED frame gefaald: %s",
                     esp_err_to_name(draw_err));
        }
    }
    // I2C-schrijfacties zijn synchroon geef de LVGL-buffer vrij na de laatste pagina.
    lv_display_flush_ready(disp);
}

static uint32_t lvgl_tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void lvgl_port_task(void *arg)
{
    lv_display_t *display = arg;
    ESP_LOGI(TAG, "Starting LVGL task");
    while (true) {
        if (display_needs_full_refresh) {
            lv_obj_invalidate(lv_display_get_screen_active(display));
        }
        uint32_t time_till_next_ms = lv_timer_handler();

        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

static lv_display_t *init_lvgl(esp_lcd_panel_io_handle_t io_handle)
{
    lv_init();
    // Lees de bestaande monotone klok in plaats van elke 5 ms een ticktimer te activeren.
    lv_tick_set_cb(lvgl_tick_ms);

    lv_display_t *display = lv_display_create(SH1106_WIDTH, SH1106_HEIGHT);
    lv_display_set_user_data(display, io_handle);

    ESP_LOGI(TAG, "Alloceer LVGL draw buffer");
    const size_t draw_buffer_sz = SH1106_WIDTH * SH1106_HEIGHT / 8 + LVGL_PALETTE_SIZE;
    void *buf = heap_caps_calloc(1, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);

    lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display, buf, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    return display;
}

void display_start(i2c_master_bus_handle_t bus)
{
    ESP_LOGI(TAG, "Initialiseer SH1106 display");
    esp_lcd_panel_io_handle_t io = sh1106_init(bus);
    lv_display_t *display = init_lvgl(io);

    // Maak de interface voordat de taak start, alle latere LVGL-aanroepen draaien in die taak.
    display_ui_create(display);

    BaseType_t created = xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, display,
                                     LVGL_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

#include "display_ui.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sensors.h"
#include "wind.h"

// #define DISPLAY_PIXEL_TEST 1
#define SENSOR_REFRESH_INTERVAL_MS 200

#if DISPLAY_PIXEL_TEST
// Test om te kijken of in het scherm uberhaupt alle pixels werken.
void display_ui_create(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_clean(screen);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    // De OLED-flushcallback keert LVGL-pixels om, dus zwart laat hier alle OLED-pixels oplichten.
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
}

#else

static lv_obj_t *measurements_label;

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void sensors_ui_cb(lv_timer_t *timer)
{
    (void)timer;
    wind_readings_t wind;
    sensor_readings_t readings;
    const bool have_wind = wind_get_latest(&wind) && wind.valid;
    const bool have_sensors = sensors_get_latest(&readings);
    char distance[32] = "Dist: -- mm";
    char temperature[32] = "Temp: -- C";
    char humidity[32] = "Hum: -- %";
    char water_temperature[32] = "Water: -- C";
    char wind_speed[32] = "Wind: -- m/s";

    if (have_sensors && readings.distance_valid) {
        snprintf(distance, sizeof(distance), "Dist: %u mm", (unsigned)readings.distance_mm);
    }
    if (have_sensors && readings.climate_valid) {
        snprintf(temperature, sizeof(temperature), "Temp: %.1f C", (double)readings.temperature_c);
        snprintf(humidity, sizeof(humidity), "Hum: %.0f %%", (double)readings.humidity_percent);
    }
    if (have_sensors && readings.water_temperature_valid) {
        snprintf(water_temperature, sizeof(water_temperature), "Water: %.1f C",
                 (double)readings.water_temperature_c);
    }
    if (have_wind) {
        snprintf(wind_speed, sizeof(wind_speed), "Wind: %.1f m/s", (double)wind.speed_ms);
    }
    char text[5 * 32 + 4];
    snprintf(text, sizeof(text), "%s\n%s\n%s\n%s\n%s", distance, temperature, humidity,
             water_temperature, wind_speed);
    set_label_text(measurements_label, text);
}

void display_ui_create(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    const int width = lv_display_get_horizontal_resolution(display);
    measurements_label = lv_label_create(screen);
    lv_obj_set_width(measurements_label, width);
    lv_label_set_long_mode(measurements_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(measurements_label, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(measurements_label, 1, LV_PART_MAIN);
    lv_label_set_text(measurements_label,
                      "Dist: -- mm\nTemp: -- C\nHum: -- %\nWater: -- C\nWind: -- m/s");
    lv_obj_set_pos(measurements_label, 0, 1);

    lv_timer_t *sensors_timer = lv_timer_create(sensors_ui_cb, SENSOR_REFRESH_INTERVAL_MS, NULL);
    assert(sensors_timer);
}

#endif

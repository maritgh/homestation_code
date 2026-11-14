#include "display_ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sensors.h"

#define SENSOR_REFRESH_INTERVAL_MS 200
#define LABEL_ROW_HEIGHT 16

static lv_obj_t *distance_label;
static lv_obj_t *temperature_label;
static lv_obj_t *humidity_label;
static lv_obj_t *water_temperature_label;

static void set_label_text(lv_obj_t *label, const char *text)
{
    // LVGL reallocates text and invalidates the label even when it is unchanged.
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_float_label(lv_obj_t *label, const char *format, float value)
{
    char text[32];
    snprintf(text, sizeof(text), format, (double)value);
    set_label_text(label, text);
}

static void sensors_ui_cb(lv_timer_t *timer)
{
    (void)timer;
    sensor_readings_t readings;
    if (!sensors_get_latest(&readings)) {
        return;
    }

    if (readings.distance_valid) {
        char text[32];
        snprintf(text, sizeof(text), "Dist: %u mm", (unsigned)readings.distance_mm);
        set_label_text(distance_label, text);
    } else {
        set_label_text(distance_label, "Dist: -- mm");
    }

    if (readings.climate_valid) {
        set_float_label(temperature_label, "Temp: %.1f C", readings.temperature_c);
        set_float_label(humidity_label, "Hum: %.1f %%", readings.humidity_percent);
    } else {
        set_label_text(temperature_label, "Temp: -- C");
        set_label_text(humidity_label, "Hum: -- %");
    }

    if (readings.water_temperature_valid) {
        set_float_label(water_temperature_label, "Water: %.1f C", readings.water_temperature_c);
    } else {
        set_label_text(water_temperature_label, "Water: -- C");
    }
}

static lv_obj_t *create_label_row(lv_obj_t *screen, int width, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, 0, y);
    return label;
}

void display_ui_create(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    const int width = lv_display_get_horizontal_resolution(display);
    distance_label = create_label_row(screen, width, 0, "Dist: -- mm");
    temperature_label = create_label_row(screen, width, LABEL_ROW_HEIGHT, "Temp: -- C");
    humidity_label = create_label_row(screen, width, 2 * LABEL_ROW_HEIGHT, "Hum: -- %");
    water_temperature_label = create_label_row(screen, width, 3 * LABEL_ROW_HEIGHT, "Water: -- C");

    lv_timer_t *sensors_timer = lv_timer_create(sensors_ui_cb, SENSOR_REFRESH_INTERVAL_MS, NULL);
    assert(sensors_timer);
}

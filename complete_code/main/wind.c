#include "wind.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#define SAMPLE_MS 200
#define MAX_READING_AGE_US 1000000
#define TARE_READINGS 10
#define PI 3.14159265358979323846f

static const char *TAG = "wind_meter";
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    gpio_num_t data;
    gpio_num_t clock;
    int32_t offset;
    float calibration;
    int64_t tare_sum;
    unsigned tare_count;
    int32_t raw;
    bool ready;
} hx711_t;

static QueueHandle_t readings_queue;
static hx711_t ns;
static hx711_t ew;
static float force_to_speed;

static esp_err_t hx711_init(hx711_t *sensor)
{
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << sensor->data,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config_t output = {
        .pin_bit_mask = 1ULL << sensor->clock, .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(sensor->clock, 0), TAG, "Set HX711 clock");
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "Configure HX711 clock");
    return gpio_config(&input);
}

/* Channel A, gain 128: 24 data pulses followed by one gain-selection pulse.
 * Prevent interrupt/task preemption from stretching the high clock pulse
 * beyond 60 us, which would power down the HX711. */
static bool hx711_read(hx711_t *sensor, int32_t *result)
{
    if (gpio_get_level(sensor->data)) return false;
    uint32_t value = 0;
    portENTER_CRITICAL(&clock_lock);
    for (int bit = 0; bit < 24; bit++) {
        gpio_set_level(sensor->clock, 1);
        esp_rom_delay_us(1);
        value = (value << 1) | gpio_get_level(sensor->data);
        gpio_set_level(sensor->clock, 0);
        esp_rom_delay_us(1);
    }
    gpio_set_level(sensor->clock, 1);
    esp_rom_delay_us(1);
    gpio_set_level(sensor->clock, 0);
    portEXIT_CRITICAL(&clock_lock);
    *result = (value & 0x800000) ? (int32_t)(value | 0xff000000) : (int32_t)value;
    return true;
}

/* Poll each amplifier independently: a missing one must not delay the other.
 * The first ten successful readings zero that axis. Offsets are kept across
 * temporary read outages. */
static void sensor_update(hx711_t *sensor)
{
    sensor->ready = false;
    if (gpio_get_level(sensor->data)) {
        return;
    }

    sensor->ready = hx711_read(sensor, &sensor->raw);
    if (sensor->ready && sensor->tare_count < TARE_READINGS) {
        sensor->tare_sum += sensor->raw;
        if (++sensor->tare_count == TARE_READINGS) {
            sensor->offset = sensor->tare_sum / TARE_READINGS;
        }
    }
}

static bool force_available(const hx711_t *sensor)
{
    return sensor->ready && sensor->tare_count == TARE_READINGS;
}

static float sensor_force(const hx711_t *sensor)
{
    return ((float)sensor->raw - sensor->offset) / sensor->calibration / 1000 * 9.80665f;
}

static void wind_task(void *arg)
{
    (void)arg;
    while (true) {
        sensor_update(&ns);
        sensor_update(&ew);
        wind_readings_t readings = {.sampled_at_us = esp_timer_get_time()};
        if (force_available(&ns) && force_available(&ew)) {
            const float force_ns = sensor_force(&ns);
            const float force_ew = sensor_force(&ew);
            readings.speed_kmh = sqrtf(hypotf(force_ns, force_ew) * force_to_speed) * 3.6f;
            readings.direction_deg = atan2f(force_ew, force_ns) * 180 / PI;
            if (readings.direction_deg < 0) readings.direction_deg += 360;
            readings.valid = isfinite(readings.speed_kmh) && isfinite(readings.direction_deg);
        }
        xQueueOverwrite(readings_queue, &readings);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

esp_err_t wind_start(void)
{
    if (readings_queue) return ESP_ERR_INVALID_STATE;
    ns = (hx711_t){.data = CONFIG_WIND_NS_DATA_GPIO, .clock = CONFIG_WIND_NS_CLOCK_GPIO,
                   .calibration = strtof(CONFIG_WIND_NS_CALIBRATION, NULL)};
    ew = (hx711_t){.data = CONFIG_WIND_EW_DATA_GPIO, .clock = CONFIG_WIND_EW_CLOCK_GPIO,
                   .calibration = strtof(CONFIG_WIND_EW_CALIBRATION, NULL)};
    const int pins[] = {ns.data, ns.clock, ew.data, ew.clock};
    for (int i = 0; i < 4; i++) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) ||
            ((i == 1 || i == 3) && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))) {
            ESP_LOGE(TAG, "Invalid HX711 GPIO; check Homestation in menuconfig");
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                ESP_LOGE(TAG, "Each HX711 signal needs a distinct GPIO");
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    const float density = strtof(CONFIG_WIND_AIR_DENSITY, NULL);
    const float radius = CONFIG_WIND_PLATE_DIAMETER_MM / 2000.0f;
    if (!isfinite(ns.calibration) || !isfinite(ew.calibration) ||
        ns.calibration == 0 || ew.calibration == 0 ||
        !isfinite(density) || density <= 0 || radius <= 0) {
        ESP_LOGE(TAG, "Invalid wind calibration, plate diameter or air density");
        return ESP_ERR_INVALID_ARG;
    }
    force_to_speed = 2 / (PI * radius * radius * density);
    ESP_RETURN_ON_ERROR(hx711_init(&ns), TAG, "Initialize NS HX711");
    ESP_RETURN_ON_ERROR(hx711_init(&ew), TAG, "Initialize EW HX711");
    readings_queue = xQueueCreate(1, sizeof(wind_readings_t));
    if (!readings_queue) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "HX711 NS DOUT=%d SCK=%d; EW DOUT=%d SCK=%d",
             (int)ns.data, (int)ns.clock, (int)ew.data, (int)ew.clock);
    ESP_LOGI(TAG, "Keep plate unloaded during startup tare (10 readings)");
    if (xTaskCreate(wind_task, "wind", 3072, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(readings_queue);
        readings_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wind_get_latest(wind_readings_t *readings)
{
    if (!readings_queue || !readings || xQueuePeek(readings_queue, readings, 0) != pdTRUE) {
        return false;
    }
    if (esp_timer_get_time() - readings->sampled_at_us > MAX_READING_AGE_US) {
        readings->valid = false;
    }
    return true;
}

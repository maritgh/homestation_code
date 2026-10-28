#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define SAMPLE_MS 200
#define HISTORY 300
#define GUST_HISTORY 120
#define PI 3.14159265358979323846f

static const char *TAG = "wind_meter";
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    gpio_num_t data, clock;
    int32_t offset;
    float calibration;
    int64_t tare_sum;
    unsigned tare_count;
    int32_t raw;
    bool ready;
} hx711_t;
typedef struct { int64_t time; float speed, direction; } sample_t;
static sample_t samples[HISTORY];
static sample_t gusts[GUST_HISTORY];
static float scratch[HISTORY];

static void hx711_init(hx711_t *sensor)
{
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << sensor->data,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config_t output = {
        .pin_bit_mask = 1ULL << sensor->clock, .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&input));
    ESP_ERROR_CHECK(gpio_config(&output));
    ESP_ERROR_CHECK(gpio_set_level(sensor->clock, 0));
}

/* Channel A, gain 128: 24 data pulses followed by one gain-selection pulse.
 * Prevent interrupt/task preemption from stretching the high clock pulse
 * beyond 60 us, which would power down the HX711. */
static bool hx711_read(hx711_t *sensor, int32_t *result)
{
    int64_t deadline = esp_timer_get_time() + 500000;
    while (gpio_get_level(sensor->data)) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
 * The first ten successful readings zero that axis; raw counts remain visible
 * during zeroing. Offsets are kept across temporary read outages. */
static void sensor_update(hx711_t *sensor)
{
    sensor->ready = false;
    if (gpio_get_level(sensor->data)) return;
    sensor->ready = hx711_read(sensor, &sensor->raw);
    if (sensor->ready && sensor->tare_count < 10) {
        sensor->tare_sum += sensor->raw;
        if (++sensor->tare_count == 10)
            sensor->offset = sensor->tare_sum / 10;
    }
}

static bool force_available(const hx711_t *sensor)
{
    return sensor->ready && sensor->tare_count == 10;
}

static float sensor_force(const hx711_t *sensor)
{
    return (sensor->raw - sensor->offset) / sensor->calibration / 1000 * 9.80665f;
}

static void print_sensor(const char *axis, const hx711_t *sensor)
{
    const char *state = !sensor->ready ? "disconnected" :
        sensor->tare_count < 10 ? "zeroing" : "ready";
    printf(",\"%s_state\":\"%s\",\"raw_%s\":", axis, state, axis);
    if (sensor->ready) printf("%ld", (long)sensor->raw);
    else printf("null");
    printf(",\"offset_%s\":", axis);
    if (sensor->tare_count == 10) printf("%ld", (long)sensor->offset);
    else printf("null");
    printf(",\"force_%s_n\":", axis);
    if (force_available(sensor)) printf("%.5f", sensor_force(sensor));
    else printf("null");
}

static int compare_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float median(int start, int end)
{
    int count = end - start;
    return count % 2 ? scratch[start + count / 2]
        : (scratch[start + count / 2 - 1] + scratch[start + count / 2]) / 2;
}

static const char *cardinal(float direction)
{
    static const char *names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return names[(int)((direction + 22.5f) / 45) % 8];
}

void app_main(void)
{
    hx711_t ns = {.data = CONFIG_WIND_NS_DATA_GPIO, .clock = CONFIG_WIND_NS_CLOCK_GPIO,
                  .calibration = strtof(CONFIG_WIND_NS_CALIBRATION, NULL)};
    hx711_t ew = {.data = CONFIG_WIND_EW_DATA_GPIO, .clock = CONFIG_WIND_EW_CLOCK_GPIO,
                  .calibration = strtof(CONFIG_WIND_EW_CALIBRATION, NULL)};
    float temperature = strtof(CONFIG_WIND_TEMPERATURE_C, NULL);
    float pressure = strtof(CONFIG_WIND_PRESSURE_HPA, NULL);
    float humidity = CONFIG_WIND_HUMIDITY_PERCENT;
    float kelvin = temperature + 273.15f;
    float vapor = humidity / 100 * 610.94f * expf(17.625f * temperature / (temperature + 243.04f));
    float density = (pressure * 100 - vapor) / (287.05f * kelvin) + vapor / (461.5f * kelvin);
    float radius = CONFIG_WIND_PLATE_DIAMETER_MM / 2000.0f;
    float area = PI * radius * radius;
    int pins[] = {ns.data, ns.clock, ew.data, ew.clock};
    for (int i = 0; i < 4; i++) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) ||
            ((i == 1 || i == 3) && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))) {
            ESP_LOGE(TAG, "Invalid GPIO; configure Wind meter in menuconfig");
            return;
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                ESP_LOGE(TAG, "Each HX711 signal needs a distinct GPIO");
                return;
            }
        }
    }
    if (!isfinite(ns.calibration) || !isfinite(ew.calibration) ||
        ns.calibration == 0 || ew.calibration == 0 || !isfinite(density) || density <= 0) {
        ESP_LOGE(TAG, "Invalid calibration or air density settings");
        return;
    }
    hx711_init(&ns);
    hx711_init(&ew);
    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "HX711 GPIOs: NS DOUT=%d SCK=%d; EW DOUT=%d SCK=%d",
             (int)ns.data, (int)ns.clock, (int)ew.data, (int)ew.clock);
    ESP_LOGI(TAG, "Keep plate unloaded during startup tare");
    unsigned index = 0, gust_index = 0;
    int64_t last_gust = 0;
    while (true) {
        int64_t cycle_start = esp_timer_get_time();
        sensor_update(&ns);
        sensor_update(&ew);
        int64_t now = esp_timer_get_time();
        printf("{\"type\":\"wind\",\"uptime_s\":%.1f", now / 1000000.0);
        print_sensor("ns", &ns);
        print_sensor("ew", &ew);
        if (force_available(&ns) && force_available(&ew)) {
            float force_ns = sensor_force(&ns);
            float force_ew = sensor_force(&ew);
            float speed = sqrtf(2 * hypotf(force_ns, force_ew) / area / density) * 3.6f;
            float direction = atan2f(force_ew, force_ns) * 180 / PI;
            if (direction < 0) direction += 360;
            samples[index++ % HISTORY] = (sample_t){now, speed, direction};
            int count = 0;
            float sin_sum = 0, cos_sum = 0;
            for (int i = 0; i < HISTORY; i++) {
                if (samples[i].time && now - samples[i].time < 60000000) {
                    scratch[count++] = samples[i].speed;
                    sin_sum += sinf(samples[i].direction * PI / 180);
                    cos_sum += cosf(samples[i].direction * PI / 180);
                }
            }
            qsort(scratch, count, sizeof(float), compare_float);
            float median_speed = median(0, count);
            float mean_direction = atan2f(sin_sum, cos_sum) * 180 / PI;
            if (mean_direction < 0) mean_direction += 360;
            if (now - last_gust >= 2500000) {
                last_gust = now;
                count = 0;
                for (int i = 0; i < HISTORY; i++) {
                    if (samples[i].time && now - samples[i].time < 2500000)
                        scratch[count++] = samples[i].speed;
                }
                if (count >= 8) {
                    qsort(scratch, count, sizeof(float), compare_float);
                    gusts[gust_index++ % GUST_HISTORY] =
                        (sample_t){now, median(count / 2, count - count / 8), 0};
                }
            }
            float gust = 0;
            for (int i = 0; i < GUST_HISTORY; i++) {
                if (gusts[i].time && now - gusts[i].time < 300000000)
                    gust = fmaxf(gust, gusts[i].speed);
            }
            printf(",\"wind_speed_kmh\":%.2f,\"wind_direction_deg\":%.1f,\"wind_cardinal\":\"%s\","
                   "\"median_speed_60s_kmh\":%.2f,\"mean_direction_60s_deg\":%.1f,"
                   "\"gust_5min_kmh\":%.2f",
                   speed, direction, cardinal(direction), median_speed, mean_direction, gust);
        } else {
            printf(",\"wind_speed_kmh\":null,\"wind_direction_deg\":null,\"wind_cardinal\":null,"
                   "\"median_speed_60s_kmh\":null,\"mean_direction_60s_deg\":null,\"gust_5min_kmh\":null");
        }
        printf(",\"temperature_c\":%.2f,\"pressure_hpa\":%.2f,\"humidity_percent\":%.1f}\n",
               temperature, pressure, humidity);
        int64_t remaining_ms = SAMPLE_MS - (esp_timer_get_time() - cycle_start) / 1000;
        vTaskDelay(pdMS_TO_TICKS(remaining_ms > 10 ? remaining_ms : 10));
    }
}

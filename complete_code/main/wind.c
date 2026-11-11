#include "wind.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board_config.h"

#define WIND_SAMPLE_INTERVAL_MS 200
#define WIND_AVERAGE_READINGS 10
#define WIND_LOG_INTERVAL_US 2000000
#define WIND_TASK_STACK_SIZE 3072
#define WIND_TASK_PRIORITY 1
#define MAX_READING_AGE_US 1000000
#define TARE_READINGS 10
// Uiteindelijke snelheidskalibratie in m/s: gekalibreerd = max(0, A * berekend + B).
#define A 0.7486f
// #define B -13.896f
#define B -3.046

#define WIND_1_CALIBRATION "-708.900330"
#define WIND_2_CALIBRATION "699.668091"
#define WIND_PLATE_DIAMETER_MM 80
#define WIND_AIR_DENSITY "1.225"

static const char *TAG = "wind_meter";
static portMUX_TYPE clock_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    gpio_num_t data;
    gpio_num_t clock;
    float calibration;
    int32_t offset;
    int64_t tare_sum;
    unsigned tare_count;
    int32_t raw;
    bool ready;
    uint32_t samples;
    uint32_t missed;
    int32_t average_readings[WIND_AVERAGE_READINGS];
    int64_t average_sum;
    unsigned average_count;
    unsigned average_next;
    int64_t last_sample_at_us;
} hx711_t;

static QueueHandle_t readings_queue;
//beide sensoren meten nu in dezelfde richting.
static hx711_t wind1;
static hx711_t wind2;
static float force_to_speed;

static esp_err_t hx711_init(hx711_t *sensor)
{
    gpio_config_t input = {
        .pin_bit_mask = 1ULL << sensor->data,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config_t output = {
        .pin_bit_mask = 1ULL << sensor->clock,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(sensor->clock, 0), TAG, "Set HX711 clock");
    ESP_RETURN_ON_ERROR(gpio_config(&output), TAG, "Configure HX711 clock");
    return gpio_config(&input);
}

/* Kanaal A, versterking 128: 24 datapulsen gevolgd door één puls voor de versterkingskeuze.
 * Voorkom dat interrupts of taakonderbrekingen de hoge klokpuls verlengen
 * tot meer dan 60 us, waardoor de HX711 zou uitschakelen. */
static bool hx711_read(hx711_t *sensor, int32_t *result)
{
    if (gpio_get_level(sensor->data)) {
        return false;
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

// Lees elke versterker apart uit en voer de nulstelling apart uit, zodat een ontbrekende versterker de andere niet blokkeert. 
static void sensor_update(hx711_t *sensor, const char *axis)
{
    sensor->ready = hx711_read(sensor, &sensor->raw);
    if (!sensor->ready) {
        sensor->missed++;
        return;
    }
    sensor->samples++;
    const int64_t now_us = esp_timer_get_time();
    // Wis oude filtergeschiedenis na uitval; behoud de nuloffset die bij het opstarten is bepaald.
    if (now_us - sensor->last_sample_at_us > MAX_READING_AGE_US) {
        sensor->average_sum = 0;
        sensor->average_count = 0;
        sensor->average_next = 0;
    }
    sensor->last_sample_at_us = now_us;
    if (sensor->tare_count < TARE_READINGS) {
        sensor->tare_sum += sensor->raw;
        if (++sensor->tare_count < TARE_READINGS) {
            return;
        }
        sensor->offset = sensor->tare_sum / TARE_READINGS;
        ESP_LOGI(TAG, "%s tare complete: offset=%" PRId32, axis, sensor->offset);
    }
    // Gebruik het beschikbare aantal metingen terwijl het middelingsvenster na de nulstelling volloopt.
    if (sensor->average_count == WIND_AVERAGE_READINGS) {
        sensor->average_sum -= sensor->average_readings[sensor->average_next];
    } else {
        sensor->average_count++;
    }
    sensor->average_readings[sensor->average_next] = sensor->raw;
    sensor->average_sum += sensor->raw;
    sensor->average_next = (sensor->average_next + 1) % WIND_AVERAGE_READINGS;
}



static bool force_available(const hx711_t *sensor)
{
    return sensor->ready && sensor->tare_count == TARE_READINGS && sensor->average_count > 0;
}

static float sensor_force(const hx711_t *sensor)
{
    const int64_t delta_sum = sensor->average_sum - (int64_t)sensor->offset * sensor->average_count;
    const float average_delta = (float)delta_sum / sensor->average_count;
    return average_delta / sensor->calibration / 1000 * 9.80665f;
}

static bool sensor_speed(const hx711_t *sensor, float *speed_ms)
{
    if (!force_available(sensor)) {
        return false;
    }
    const float calculated_speed_ms = sqrtf(fabsf(sensor_force(sensor)) * force_to_speed);
    const float calibrated_speed_ms = A * calculated_speed_ms + B;
    if (!isfinite(calibrated_speed_ms)) {
        return false;
    }
    *speed_ms = fmaxf(0.0f, calibrated_speed_ms);
    return true;
}

static void wind_task(void *arg)
{
    (void)arg;
    int64_t last_log_us = 0;
    while (true) {
        sensor_update(&wind1, "wind1");
        sensor_update(&wind2, "Wind2");
        wind_readings_t readings = {.sampled_at_us = esp_timer_get_time()};
        float speed_1 = 0.0f;
        float speed_2 = 0.0f;
        const bool have_1 = sensor_speed(&wind1, &speed_1);
        const bool have_2= sensor_speed(&wind2, &speed_2);
        if (have_1 || have_2) {
            // Middel de beschikbare onafhankelijke snelheden; tel een ontbrekende sensor nooit als nul mee.
            const float speed_sum = (have_1 ? speed_1 : 0.0f) + (have_2 ? speed_2 : 0.0f);
            const unsigned sensor_count = (unsigned)have_1 + (unsigned)have_2;
            readings.speed_ms = speed_sum / sensor_count;
            readings.valid = isfinite(readings.speed_ms);
        }
        xQueueOverwrite(readings_queue, &readings);
        if (last_log_us == 0 || readings.sampled_at_us - last_log_us >= WIND_LOG_INTERVAL_US) {
            // sensor_log(&wind1, "WIND1");
            // sensor_log(&wind2, "WIND2");
            if (readings.valid && have_1 && have_2) {
                ESP_LOGI(TAG, "Wind speed=%.2f m/s (WIND1=%.2f m/s WIND2=%.2f m/s)",
                         (double)readings.speed_ms, (double)speed_1, (double)speed_2);
            } else if (readings.valid) {
                ESP_LOGW(TAG, "Wind speed=%.2f m/s using %s sensor only",
                         (double)readings.speed_ms, have_1 ? "WIND1" : "WIND2");
            } else if (force_available(&wind1) || force_available(&wind2)) {
                ESP_LOGW(TAG, "Wind invalid: calculated speed is not finite");
            } else {
                ESP_LOGW(TAG, "Wind unavailable: at least one sensor needs a ready sample and completed tare (wind1=%u/%u wind2=%u/%u)",
                         wind1.tare_count, (unsigned)TARE_READINGS, wind2.tare_count, (unsigned)TARE_READINGS);
            }
            last_log_us = readings.sampled_at_us;
        }
        vTaskDelay(pdMS_TO_TICKS(WIND_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t wind_start(void)
{
    if (readings_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    wind1 = (hx711_t){.data = WIND_1_DATA_GPIO,
                   .clock = WIND_2_CLOCK_GPIO,
                   .calibration = strtof(WIND_1_CALIBRATION, NULL)};
    wind2 = (hx711_t){.data = WIND_2_DATA_GPIO,
                   .clock = WIND_2_CLOCK_GPIO,
                   .calibration = strtof(WIND_2_CALIBRATION, NULL)};
    const int pins[] = {wind1.data, wind1.clock, wind2.data, wind2.clock};
    for (int i = 0; i < 4; i++) {
        if (!GPIO_IS_VALID_GPIO(pins[i]) ||
            ((i == 1 || i == 3) && !GPIO_IS_VALID_OUTPUT_GPIO(pins[i]))) {
            ESP_LOGE(TAG, "Invalid HX711 GPIO; check board_config.h");
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (pins[i] == pins[j]) {
                ESP_LOGE(TAG, "Each HX711 signal needs a distinct GPIO");
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    const float density = strtof(WIND_AIR_DENSITY, NULL);
    const float radius = WIND_PLATE_DIAMETER_MM / 2000.0f;
    if (!isfinite(wind1.calibration) || !isfinite(wind2.calibration) || wind1.calibration == 0 ||
        wind2.calibration == 0 || !isfinite(density) || density <= 0 || radius <= 0) {
        ESP_LOGE(TAG, "Invalid wind calibration, plate diameter or air density");
        return ESP_ERR_INVALID_ARG;
    }
    force_to_speed = 2 / ((float)M_PI * radius * radius * density);
    ESP_RETURN_ON_ERROR(hx711_init(&wind1), TAG, "Initialize WIND1 HX711");
    ESP_RETURN_ON_ERROR(hx711_init(&wind2), TAG, "Initialize WIND2 HX711");
    readings_queue = xQueueCreate(1, sizeof(wind_readings_t));
    if (!readings_queue) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "HX711 WIND1 DOUT=%d SCK=%d; WIND2 DOUT=%d SCK=%d", (int)wind1.data, (int)wind1.clock,
             (int)wind2.data, (int)wind2.clock);
    ESP_LOGI(TAG, "Keep plates unloaded during startup tare (%u readings per sensor)", (unsigned)TARE_READINGS);
    ESP_LOGI(TAG, "Noise filter: moving average of %u readings per sensor", (unsigned)WIND_AVERAGE_READINGS);
    ESP_LOGI(TAG, "Aligned sensors: average independent wind speeds; direction disabled");
    if (xTaskCreate(wind_task, "wind", WIND_TASK_STACK_SIZE, NULL, WIND_TASK_PRIORITY, NULL) !=
        pdPASS) {
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

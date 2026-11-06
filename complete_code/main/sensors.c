#include "sensors.h"

#include "board_config.h"
#include "i2c_shared.h"
#include "vl53l4cd.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <rom/ets_sys.h>

#define DS18B20_GPIO BOARD_DS18B20_GPIO
#define DS18B20_CONVERSION_TIME_MS 750
#define DS18B20_CMD_SKIP_ROM 0xCCU
#define DS18B20_CMD_CONVERT_TEMPERATURE 0x44U
#define DS18B20_CMD_READ_SCRATCHPAD 0xBEU

#define TOF_I2C_ADDRESS 0x29
#define SHT45_I2C_ADDRESS 0x44
#define SENSOR_I2C_CLOCK_HZ 100000
#define SENSOR_I2C_TIMEOUT_MS 100
#define CLIMATE_READ_INTERVAL_US 1000000
#define WATER_READ_INTERVAL_US 2000000
#define TOF_RETRY_INTERVAL_US 5000000
#define TOF_STALL_TIMEOUT_US 1000000
#define SENSOR_LOG_INTERVAL_US 5000000
#define SENSOR_POLL_INTERVAL_MS 100
#define SENSOR_TASK_STACK_SIZE 4096
#define SENSOR_TASK_PRIORITY 1

static const char *TAG = "sensors";
static i2c_master_dev_handle_t tof;
static i2c_master_dev_handle_t sht;
static i2c_master_bus_handle_t sensor_bus;
static QueueHandle_t readings_queue;

static void ds18b20_set_bus_mode(gpio_mode_t mode) { gpio_set_direction(DS18B20_GPIO, mode); }

static void ds18b20_write_bit(bool bit)
{
    ds18b20_set_bus_mode(GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DS18B20_GPIO, 0);
    ets_delay_us(2);
    gpio_set_level(DS18B20_GPIO, bit ? 1 : 0);
    ets_delay_us(60);
    ds18b20_set_bus_mode(GPIO_MODE_INPUT);
}

static uint8_t ds18b20_read_bit(void)
{
    ds18b20_set_bus_mode(GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DS18B20_GPIO, 0);
    ets_delay_us(2);
    ds18b20_set_bus_mode(GPIO_MODE_INPUT);
    ets_delay_us(10);
    uint8_t bit = gpio_get_level(DS18B20_GPIO);
    ets_delay_us(50);
    return bit;
}

static void ds18b20_write_byte(uint8_t value)
{
    for (int bit = 0; bit < 8; ++bit) {
        ds18b20_write_bit((value >> bit) & 0x01U);
    }
}

static uint8_t ds18b20_read_byte(void)
{
    uint8_t value = 0;
    for (int bit = 0; bit < 8; ++bit) {
        value |= ((uint8_t)ds18b20_read_bit() << bit);
    }
    return value;
}

static bool ds18b20_reset(void)
{
    ds18b20_set_bus_mode(GPIO_MODE_OUTPUT_OD);
    gpio_set_level(DS18B20_GPIO, 0);
    ets_delay_us(480);
    ds18b20_set_bus_mode(GPIO_MODE_INPUT);
    ets_delay_us(70);
    bool presence = gpio_get_level(DS18B20_GPIO) == 0;
    ets_delay_us(410);
    return presence;
}

static esp_err_t ds18b20_start_conversion(void)
{
    if (!ds18b20_reset()) {
        return ESP_ERR_NOT_FOUND;
    }

    ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
    ds18b20_write_byte(DS18B20_CMD_CONVERT_TEMPERATURE);
    return ESP_OK;
}

static esp_err_t ds18b20_read(sensor_readings_t *readings)
{
    if (!ds18b20_reset()) {
        return ESP_ERR_NOT_FOUND;
    }

    ds18b20_write_byte(DS18B20_CMD_SKIP_ROM);
    ds18b20_write_byte(DS18B20_CMD_READ_SCRATCHPAD);

    uint8_t scratchpad[9] = {0};
    for (int i = 0; i < (int)sizeof(scratchpad); ++i) {
        scratchpad[i] = ds18b20_read_byte();
    }

    const int16_t raw_temp = (int16_t)((uint16_t)scratchpad[0] | ((uint16_t)scratchpad[1] << 8));
    readings->water_temperature_c = raw_temp / 16.0f;
    return ESP_OK;
}

typedef struct {
    bool converting;
    bool reported;
    int64_t ready_at;
    int64_t next_start;
} ds18b20_state_t;

static void ds18b20_update(ds18b20_state_t *state, sensor_readings_t *readings)
{
    const int64_t now = esp_timer_get_time();
    esp_err_t err;
    if (state->converting) {
        if (now < state->ready_at) {
            return;
        }
        err = ds18b20_read(readings);
        state->converting = false;
    } else {
        if (now < state->next_start) {
            return;
        }
        err = ds18b20_start_conversion();
        state->next_start = now + WATER_READ_INTERVAL_US;
        if (err == ESP_OK) {
            state->converting = true;
            // The other sensors keep running while the DS18B20 converts.
            state->ready_at = esp_timer_get_time() + DS18B20_CONVERSION_TIME_MS * 1000;
            return;
        }
    }

    const bool valid = err == ESP_OK;
    if (!state->reported || readings->water_temperature_valid != valid) {
        ESP_LOGI(TAG, "DS18B20 on GPIO %d: %s", DS18B20_GPIO, esp_err_to_name(err));
        state->reported = true;
    }
    readings->water_temperature_valid = valid;
}

static void scan_bus(void)
{
    unsigned found = 0;
    ESP_LOGI(TAG, "I2C scan (7-bit addresses)");

    for (unsigned addr = 0x08; addr <= 0x77; ++addr) {
        shared_i2c_acquire();
        esp_err_t err = i2c_master_probe(sensor_bus, addr, 50);
        shared_i2c_release();

        if (err == ESP_OK) {
            ESP_LOGI(TAG, "I2C device at 0x%02x", addr);
            ++found;
        } else if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "I2C scan 0x%02x: %s", addr, esp_err_to_name(err));
        }

        vTaskDelay(1);
    }

    ESP_LOGI(TAG, "I2C scan complete: %u devices", found);
}

static uint8_t sht45_crc8(const uint8_t *data)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht45_read(sensor_readings_t *readings)
{
    const uint8_t command = 0xfd;
    uint8_t data[6] = {0};

    shared_i2c_acquire();
    esp_err_t err = i2c_master_transmit(sht, &command, sizeof(command), SENSOR_I2C_TIMEOUT_MS);
    shared_i2c_release();
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(10) + 1);

    shared_i2c_acquire();
    err = i2c_master_receive(sht, data, sizeof(data), SENSOR_I2C_TIMEOUT_MS);
    shared_i2c_release();
    if (err != ESP_OK) {
        return err;
    }

    if (sht45_crc8(data) != data[2] || sht45_crc8(data + 3) != data[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
    const uint16_t raw_rh = ((uint16_t)data[3] << 8) | data[4];
    readings->temperature_c = -45.0f + 175.0f * raw_t / 65535.0f;

    const float humidity = -6.0f + 125.0f * raw_rh / 65535.0f;
    readings->humidity_percent = humidity < 0.0f ? 0.0f : (humidity > 100.0f ? 100.0f : humidity);
    return ESP_OK;
}

static void log_readings(const sensor_readings_t *readings)
{
    if (readings->climate_valid) {
        ESP_LOGI(TAG, "Temperature %.1f C, humidity %.1f %%", (double)readings->temperature_c,
                 (double)readings->humidity_percent);
    }
    if (readings->water_temperature_valid) {
        ESP_LOGI(TAG, "Water temperature %.1f C", (double)readings->water_temperature_c);
    }
    if (readings->distance_valid) {
        ESP_LOGI(TAG, "Distance %u mm", (unsigned)readings->distance_mm);
    }
}

static void sensors_task(void *arg)
{
    (void)arg;
    scan_bus();
    sensor_readings_t readings = {0};
    bool tof_running = false;
    int64_t next_climate_check = 0;
    ds18b20_state_t water_state = {0};
    int64_t next_tof_retry = 0;
    int64_t last_tof_activity = 0;
    int64_t next_log = 0;

    while (true) {
        const int64_t now = esp_timer_get_time();

        if (now >= next_climate_check) {
            const bool was_valid = readings.climate_valid;
            esp_err_t err = sht45_read(&readings);
            readings.climate_valid = err == ESP_OK;

            if (was_valid != readings.climate_valid || next_climate_check == 0) {
                ESP_LOGI(TAG, "SHT45 0x44: %s", esp_err_to_name(err));
            }

            next_climate_check = now + CLIMATE_READ_INTERVAL_US;
        }

        ds18b20_update(&water_state, &readings);

        if (!tof_running && now >= next_tof_retry) {
            shared_i2c_acquire();
            esp_err_t err = i2c_master_probe(sensor_bus, TOF_I2C_ADDRESS, SENSOR_I2C_TIMEOUT_MS);
            shared_i2c_release();

            if (err == ESP_OK) {
                err = vl53l4cd_init(tof);
            }

            tof_running = err == ESP_OK;
            ESP_LOGI(TAG, "VL53L4CD 0x29: %s", esp_err_to_name(err));
            last_tof_activity = now;
            next_tof_retry = now + TOF_RETRY_INTERVAL_US;
        }

        if (tof_running) {
            bool valid = false;
            esp_err_t err = vl53l4cd_read(tof, &readings.distance_mm, &valid);

            if (err == ESP_OK) {
                readings.distance_valid = valid;
                last_tof_activity = esp_timer_get_time();
            } else if (err != ESP_ERR_NOT_FINISHED ||
                       esp_timer_get_time() - last_tof_activity > TOF_STALL_TIMEOUT_US) {
                readings.distance_valid = false;
                tof_running = false;
                next_tof_retry = esp_timer_get_time() + TOF_RETRY_INTERVAL_US;
                ESP_LOGW(TAG, "VL53L4CD read failed/stalled: %s", esp_err_to_name(err));
            }
        }

        if (now >= next_log) {
            log_readings(&readings);
            next_log = now + SENSOR_LOG_INTERVAL_US;
        }

        xQueueOverwrite(readings_queue, &readings);
        vTaskDelay(pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS));
    }
}

esp_err_t sensors_start(i2c_master_bus_handle_t bus)
{
    sensor_bus = bus;

    gpio_reset_pin(DS18B20_GPIO);
    gpio_set_pull_mode(DS18B20_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(DS18B20_GPIO, GPIO_MODE_INPUT);

    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOF_I2C_ADDRESS,
        .scl_speed_hz = SENSOR_I2C_CLOCK_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &tof), TAG, "Add ToF");

    config.device_address = SHT45_I2C_ADDRESS;
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &sht);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(tof);
        return err;
    }

    readings_queue = xQueueCreate(1, sizeof(sensor_readings_t));
    if (!readings_queue) {
        i2c_master_bus_rm_device(sht);
        i2c_master_bus_rm_device(tof);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(sensors_task, "sensors", SENSOR_TASK_STACK_SIZE, NULL, SENSOR_TASK_PRIORITY,
                    NULL) == pdPASS) {
        return ESP_OK;
    }

    vQueueDelete(readings_queue);
    readings_queue = NULL;
    i2c_master_bus_rm_device(sht);
    i2c_master_bus_rm_device(tof);
    return ESP_ERR_NO_MEM;
}

bool sensors_get_latest(sensor_readings_t *readings)
{
    return readings_queue && readings && xQueuePeek(readings_queue, readings, 0) == pdTRUE;
}

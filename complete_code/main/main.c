#include "airco.h"
#include "board_config.h"
#include "display.h"
#include "mqtt.h"
#include "sensors.h"
#include "wind.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "main";

static void log_startup_error(const char *module, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s startup gefaald: %s", module, esp_err_to_name(err));
    }
}

void app_main(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    display_start(bus);
    ESP_ERROR_CHECK(sensors_start(bus));

    log_startup_error("Wind sensor", wind_start());
    log_startup_error("IR", airco_start());
    log_startup_error("Wi-Fi/MQTT", home_station_mqtt_start());
}

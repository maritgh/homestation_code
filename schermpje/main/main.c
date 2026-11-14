#include "display.h"
#include "mqtt.h"
#include "sensors.h"

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"

#define I2C_BUS_PORT 0
#define I2C_SDA_PIN 3
#define I2C_SCL_PIN 4

void app_main(void)
{
    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_BUS_PORT,
        .sda_io_num = I2C_SDA_PIN,
        .scl_io_num = I2C_SCL_PIN,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    display_start(bus);
    ESP_ERROR_CHECK(sensors_start(bus));

    const esp_err_t network_err = home_station_mqtt_start();
    if (network_err != ESP_OK) {
        ESP_LOGE("main", "Wi-Fi/MQTT startup failed: %s", esp_err_to_name(network_err));
    }
}

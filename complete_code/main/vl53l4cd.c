/**
 ******************************************************************************
 * @file    vl53l4cd_api.cpp
 * @author  STMicroelectronics
 * @version V1.0.0
 * @date    29 November 2021
 * @brief   Implementation of the VL53L4CD APIs.
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; COPYRIGHT(c) 2021 STMicroelectronics</center></h2>
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *   1. Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright notice,
 *      this list of conditions and the following disclaimer in the documentation
 *      and/or other materials provided with the distribution.
 *   3. Neither the name of STMicroelectronics nor the names of its contributors
 *      may be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************
 */

// ESP-IDF adaptation: initialization and continuous ranging only.
// Source: https://github.com/stm32duino/VL53L4CD/blob/main/src/vl53l4cd_api.cpp
#include "vl53l4cd.h"
#include "i2c_shared.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const uint8_t VL53L4CD_DEFAULT_CONFIGURATION[] = {
  0x12, /* 0x2d : set bit 2 and 5 to 1 for fast plus mode (1MHz I2C),
   else don't touch */
  0x00, /* 0x2e : bit 0 if I2C pulled up at 1.8V, else set bit 0 to 1
   (pull up at AVDD) */
  0x00, /* 0x2f : bit 0 if GPIO pulled up at 1.8V, else set bit 0 to 1
  (pull up at AVDD) */
  0x11, /* 0x30 : set bit 4 to 0 for active high interrupt and 1 for active low
  (bits 3:0 must be 0x1), use SetInterruptPolarity() */
  0x02, /* 0x31 : bit 1 = interrupt depending on the polarity,
  use CheckForDataReady() */
  0x00, /* 0x32 : not user-modifiable */
  0x02, /* 0x33 : not user-modifiable */
  0x08, /* 0x34 : not user-modifiable */
  0x00, /* 0x35 : not user-modifiable */
  0x08, /* 0x36 : not user-modifiable */
  0x10, /* 0x37 : not user-modifiable */
  0x01, /* 0x38 : not user-modifiable */
  0x01, /* 0x39 : not user-modifiable */
  0x00, /* 0x3a : not user-modifiable */
  0x00, /* 0x3b : not user-modifiable */
  0x00, /* 0x3c : not user-modifiable */
  0x00, /* 0x3d : not user-modifiable */
  0xff, /* 0x3e : not user-modifiable */
  0x00, /* 0x3f : not user-modifiable */
  0x0F, /* 0x40 : not user-modifiable */
  0x00, /* 0x41 : not user-modifiable */
  0x00, /* 0x42 : not user-modifiable */
  0x00, /* 0x43 : not user-modifiable */
  0x00, /* 0x44 : not user-modifiable */
  0x00, /* 0x45 : not user-modifiable */
  0x20, /* 0x46 : interrupt configuration 0->level low detection, 1-> level high,
  2-> Out of window, 3->In window, 0x20-> New sample ready , TBC */
  0x0b, /* 0x47 : not user-modifiable */
  0x00, /* 0x48 : not user-modifiable */
  0x00, /* 0x49 : not user-modifiable */
  0x02, /* 0x4a : not user-modifiable */
  0x14, /* 0x4b : not user-modifiable */
  0x21, /* 0x4c : not user-modifiable */
  0x00, /* 0x4d : not user-modifiable */
  0x00, /* 0x4e : not user-modifiable */
  0x05, /* 0x4f : not user-modifiable */
  0x00, /* 0x50 : not user-modifiable */
  0x00, /* 0x51 : not user-modifiable */
  0x00, /* 0x52 : not user-modifiable */
  0x00, /* 0x53 : not user-modifiable */
  0xc8, /* 0x54 : not user-modifiable */
  0x00, /* 0x55 : not user-modifiable */
  0x00, /* 0x56 : not user-modifiable */
  0x38, /* 0x57 : not user-modifiable */
  0xff, /* 0x58 : not user-modifiable */
  0x01, /* 0x59 : not user-modifiable */
  0x00, /* 0x5a : not user-modifiable */
  0x08, /* 0x5b : not user-modifiable */
  0x00, /* 0x5c : not user-modifiable */
  0x00, /* 0x5d : not user-modifiable */
  0x01, /* 0x5e : not user-modifiable */
  0xcc, /* 0x5f : not user-modifiable */
  0x07, /* 0x60 : not user-modifiable */
  0x01, /* 0x61 : not user-modifiable */
  0xf1, /* 0x62 : not user-modifiable */
  0x05, /* 0x63 : not user-modifiable */
  0x00, /* 0x64 : Sigma threshold MSB (mm in 14.2 format for MSB+LSB),
   use SetSigmaThreshold(), default value 90 mm  */
  0xa0, /* 0x65 : Sigma threshold LSB */
  0x00, /* 0x66 : Min count Rate MSB (MCPS in 9.7 format for MSB+LSB),
   use SetSignalThreshold() */
  0x80, /* 0x67 : Min count Rate LSB */
  0x08, /* 0x68 : not user-modifiable */
  0x38, /* 0x69 : not user-modifiable */
  0x00, /* 0x6a : not user-modifiable */
  0x00, /* 0x6b : not user-modifiable */
  0x00, /* 0x6c : Intermeasurement period MSB, 32 bits register,
   use SetIntermeasurementInMs() */
  0x00, /* 0x6d : Intermeasurement period */
  0x0f, /* 0x6e : Intermeasurement period */
  0x89, /* 0x6f : Intermeasurement period LSB */
  0x00, /* 0x70 : not user-modifiable */
  0x00, /* 0x71 : not user-modifiable */
  0x00, /* 0x72 : distance threshold high MSB (in mm, MSB+LSB),
   use SetD:tanceThreshold() */
  0x00, /* 0x73 : distance threshold high LSB */
  0x00, /* 0x74 : distance threshold low MSB ( in mm, MSB+LSB),
   use SetD:tanceThreshold() */
  0x00, /* 0x75 : distance threshold low LSB */
  0x00, /* 0x76 : not user-modifiable */
  0x01, /* 0x77 : not user-modifiable */
  0x07, /* 0x78 : not user-modifiable */
  0x05, /* 0x79 : not user-modifiable */
  0x06, /* 0x7a : not user-modifiable */
  0x06, /* 0x7b : not user-modifiable */
  0x00, /* 0x7c : not user-modifiable */
  0x00, /* 0x7d : not user-modifiable */
  0x02, /* 0x7e : not user-modifiable */
  0xc7, /* 0x7f : not user-modifiable */
  0xff, /* 0x80 : not user-modifiable */
  0x9B, /* 0x81 : not user-modifiable */
  0x00, /* 0x82 : not user-modifiable */
  0x00, /* 0x83 : not user-modifiable */
  0x00, /* 0x84 : not user-modifiable */
  0x01, /* 0x85 : not user-modifiable */
  0x00, /* 0x86 : clear interrupt, use ClearInterrupt() */
  0x00  /* 0x87 : start ranging, use StartRanging() or StopRanging(),
   If you want an automatic start after VL53L4CD_init() call,
    put 0x40 in location 0x87 */
};

static const char *TAG = "vl53l4cd";
#define CHECK(call) ESP_RETURN_ON_ERROR((call), TAG, "Sensor communication/init failed")

static esp_err_t read_reg(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[] = {reg >> 8, reg & 0xff};
    shared_i2c_acquire();
    esp_err_t err = i2c_master_transmit_receive(dev, addr, sizeof(addr), data, len, 100);
    shared_i2c_release();
    return err;
}

static esp_err_t transmit(i2c_master_dev_handle_t dev, const uint8_t *data, size_t len)
{
    shared_i2c_acquire();
    esp_err_t err = i2c_master_transmit(dev, data, len, 100);
    shared_i2c_release();
    return err;
}

static esp_err_t write_byte(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t value)
{
    uint8_t data[] = {reg >> 8, reg & 0xff, value};
    return transmit(dev, data, sizeof(data));
}

static esp_err_t write_word(i2c_master_dev_handle_t dev, uint16_t reg, uint16_t value)
{
    uint8_t data[] = {reg >> 8, reg & 0xff, value >> 8, value & 0xff};
    return transmit(dev, data, sizeof(data));
}

static esp_err_t data_ready(i2c_master_dev_handle_t dev, bool *ready)
{
    uint8_t polarity, status;
    CHECK(read_reg(dev, 0x0030, &polarity, 1));
    CHECK(read_reg(dev, 0x0031, &status, 1));
    *ready = (status & 1) == !(polarity & 0x10);
    return ESP_OK;
}

static esp_err_t wait_ready(i2c_master_dev_handle_t dev)
{
    int64_t deadline = esp_timer_get_time() + 1000000;
    do {
        bool ready;
        CHECK(data_ready(dev, &ready));
        if (ready) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t vl53l4cd_init(i2c_master_dev_handle_t dev)
{
    uint8_t id[2], boot = 0;
    CHECK(read_reg(dev, 0x010f, id, sizeof(id)));
    if (id[0] != 0xeb || id[1] != 0xaa) {
        ESP_LOGW(TAG, "Unexpected model ID: %02x%02x", id[0], id[1]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    // Stop a previous run too: the sensor may stay powered during an ESP reset.
    CHECK(write_byte(dev, 0x0087, 0));
    int64_t deadline = esp_timer_get_time() + 1000000;
    do {
        CHECK(read_reg(dev, 0x00e5, &boot, 1));
        if (boot == 3) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline);
    if (boot != 3) {
        return ESP_ERR_TIMEOUT;
    }
    for (unsigned i = 0; i < sizeof(VL53L4CD_DEFAULT_CONFIGURATION); ++i) {
        CHECK(write_byte(dev, 0x002d + i, VL53L4CD_DEFAULT_CONFIGURATION[i]));
    }
    CHECK(write_byte(dev, 0x0087, 0x40)); // VHV calibration
    CHECK(wait_ready(dev));
    CHECK(write_byte(dev, 0x0086, 1));
    CHECK(write_byte(dev, 0x0087, 0));
    CHECK(write_byte(dev, 0x0008, 9));
    CHECK(write_byte(dev, 0x000b, 0));
    CHECK(write_word(dev, 0x0024, 0x0500));

    // ST ULD SetRangeTiming(50, 0): 50 ms budget, continuous mode.
    uint8_t osc_bytes[2];
    CHECK(read_reg(dev, 0x0006, osc_bytes, sizeof(osc_bytes)));
    uint16_t osc = ((uint16_t)osc_bytes[0] << 8) | osc_bytes[1];
    if (!osc) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t macro_period = (2304U * (0x40000000U / osc)) >> 6;
    const uint8_t continuous[] = {0x00, 0x6c, 0, 0, 0, 0};
    CHECK(transmit(dev, continuous, sizeof(continuous)));
    const uint32_t budget = (50000U - 2500U) << 12;
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t period = (macro_period * (i == 0 ? 16U : 12U)) >> 6;
        if (!period) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t mantissa = (budget + (period >> 1)) / period - 1;
        uint16_t exponent = 0;
        while (mantissa > 255) {
            mantissa >>= 1;
            ++exponent;
        }
        CHECK(write_word(dev, i == 0 ? 0x005e : 0x0061,
                         (exponent << 8) | mantissa));
    }
    CHECK(write_byte(dev, 0x0087, 0x21));
    CHECK(wait_ready(dev));
    return write_byte(dev, 0x0086, 1);
}

esp_err_t vl53l4cd_read(i2c_master_dev_handle_t dev, uint16_t *mm, bool *valid)
{
    *valid = false;
    bool ready;
    CHECK(data_ready(dev, &ready));
    if (!ready) {
        return ESP_ERR_NOT_FINISHED;
    }
    // Read status, SPAD count and distance in one snapshot (0x89..0x97).
    uint8_t result[15];
    CHECK(read_reg(dev, 0x0089, result, sizeof(result)));
    CHECK(write_byte(dev, 0x0086, 1));
    // Raw status 9 maps to ULD range_status 0 (valid measurement).
    *valid = (result[0] & 0x1f) == 9 && result[3] != 0;
    *mm = ((uint16_t)result[13] << 8) | result[14];
    return ESP_OK;
}

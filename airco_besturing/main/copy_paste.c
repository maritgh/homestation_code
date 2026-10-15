#include <stdbool.h>
#include <string.h>

#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define IR_RX_GPIO          GPIO_NUM_8
#define IR_TX_GPIO          GPIO_NUM_10

#define RMT_RESOLUTION_HZ   1000000   // 1 tick = 1 microseconde
#define RX_SYMBOL_COUNT     512

static const char *TAG = "ir_repeat_test";

static rmt_channel_handle_t rx_channel;
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t copy_encoder;
static QueueHandle_t rx_queue;

static rmt_symbol_word_t rx_symbols[RX_SYMBOL_COUNT];
static rmt_symbol_word_t tx_symbols[RX_SYMBOL_COUNT];

static bool ir_rx_done_callback(
    rmt_channel_handle_t channel,
    const rmt_rx_done_event_data_t *event_data,
    void *user_data)
{
    BaseType_t task_woken = pdFALSE;
    QueueHandle_t queue = (QueueHandle_t)user_data;

    xQueueSendFromISR(queue, event_data, &task_woken);

    return task_woken == pdTRUE;
}

void app_main(void)
{
    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Kan de ontvangstruimte niet aanmaken");
        return;
    }

    // RMT-ontvanger
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .flags.with_dma = true,
        .flags.invert_in = false,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_config, &rx_channel));

    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = ir_rx_done_callback,
    };
    ESP_ERROR_CHECK(
        rmt_rx_register_event_callbacks(rx_channel, &callbacks, rx_queue)
    );

    // RMT-zender
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));

    // Gebruik voorlopig een draaggolf van 38 kHz.
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

    // Deze encoder verstuurt de ruwe RMT-symbolen.
    rmt_copy_encoder_config_t encoder_config = {0};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &copy_encoder));

    // De stilte beëindigt de opname.
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 30000000,
    };

    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_ERROR_CHECK(rmt_enable(tx_channel));

    ESP_LOGI(TAG, "Klaar. Druk nu één keer op de aan-knop.");
    ESP_LOGI(TAG, "Het signaal wordt na 2 seconden opnieuw verzonden.");

    ESP_ERROR_CHECK(
        rmt_receive(
            rx_channel,
            rx_symbols,
            sizeof(rx_symbols),
            &receive_config
        )
    );

    while (1) {
        rmt_rx_done_event_data_t rx_data;

        if (xQueueReceive(rx_queue, &rx_data, portMAX_DELAY) == pdTRUE) {
            size_t count = rx_data.num_symbols;

            if (count == 0 || count > RX_SYMBOL_COUNT) {
                ESP_LOGE(TAG, "Ongeldig aantal symbolen: %u",
                         (unsigned)count);
            } else {
                ESP_LOGI(TAG, "%u symbolen ontvangen",
                         (unsigned)count);

                // Bewaar de opname en keer de niveaus om.
                // IR-ontvangers geven een IR-puls meestal als laag niveau door.
                for (size_t i = 0; i < count; i++) {
                    tx_symbols[i] = rx_data.received_symbols[i];
                    tx_symbols[i].level0 = !tx_symbols[i].level0;
                    tx_symbols[i].level1 = !tx_symbols[i].level1;
                }

                vTaskDelay(pdMS_TO_TICKS(2000));

                ESP_LOGI(TAG, "Signaal verzenden");

                rmt_transmit_config_t transmit_config = {
                    .loop_count = 0,
                };

                ESP_ERROR_CHECK(
                    rmt_transmit(
                        tx_channel,
                        copy_encoder,
                        tx_symbols,
                        count * sizeof(rmt_symbol_word_t),
                        &transmit_config
                    )
                );

                ESP_ERROR_CHECK(
                    rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(5000))
                );

                ESP_LOGI(TAG, "Verzenden klaar");
            }

            // Luister opnieuw naar de volgende knop.
            ESP_ERROR_CHECK(
                rmt_receive(
                    rx_channel,
                    rx_symbols,
                    sizeof(rx_symbols),
                    &receive_config
                )
            );
        }
    }
}
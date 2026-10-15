#include <inttypes.h>

#include "driver/rmt_rx.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define IR_RX_GPIO         GPIO_NUM_8
#define RMT_RESOLUTION_HZ  1000000  // 1 tick = 1 microseconde
#define RX_SYMBOL_COUNT    512

static const char *TAG = "ir_capture";

static rmt_channel_handle_t rx_channel;
static QueueHandle_t rx_queue;

/*
 * De RMT-callback wordt aangeroepen wanneer een opname klaar is.
 * In de callback sturen we alleen de gegevens naar de hoofdtaak.
 */
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
    ESP_LOGI(TAG, "IR-ontvanger starten op GPIO %d", IR_RX_GPIO);

    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Kan ontvangstruimte niet aanmaken");
        return;
    }

    // Maak het RMT-ontvangstkanaal aan.
    rmt_rx_channel_config_t rx_channel_config = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .flags.with_dma = true,
        .flags.invert_in = false,
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_config, &rx_channel));

    // Registreer de callback die wordt aangeroepen na een ontvangst.
    rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = ir_rx_done_callback,
    };

    ESP_ERROR_CHECK(
        rmt_rx_register_event_callbacks(rx_channel, &callbacks, rx_queue)
    );

    /*
     * Pulsen korter dan 2 us worden als storing genegeerd.
     * Een stilte van 30 ms beëindigt de opname.
     */
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 30000000,
    };

    static rmt_symbol_word_t raw_symbols[RX_SYMBOL_COUNT];

    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    ESP_LOGI(TAG, "Klaar. Richt de afstandsbediening op de ontvanger.");
    ESP_LOGI(TAG, "Druk op een knop; de pulsen verschijnen hieronder.");

    // Start de eerste opname.
    ESP_ERROR_CHECK(
        rmt_receive(
            rx_channel,
            raw_symbols,
            sizeof(raw_symbols),
            &receive_config
        )
    );

    while (1) {
        rmt_rx_done_event_data_t rx_data;

        // Wacht tot een IR-signaal is ontvangen.
        if (xQueueReceive(rx_queue, &rx_data, portMAX_DELAY) == pdTRUE) {
            printf("\n--- IR-opname: %u symbolen ---\n",
                   (unsigned)rx_data.num_symbols);
            printf("index,level0,duration0_us,level1,duration1_us\n");

            for (size_t i = 0; i < rx_data.num_symbols; i++) {
                const rmt_symbol_word_t *symbol = &rx_data.received_symbols[i];

                printf("%u,%u,%" PRIu32 ",%u,%" PRIu32 "\n",
                       (unsigned)i,
                       (unsigned)symbol->level0,
                       (uint32_t)symbol->duration0,
                       (unsigned)symbol->level1,
                       (uint32_t)symbol->duration1);
            }

            printf("--- einde opname ---\n");

            // Start opnieuw met luisteren naar de volgende knop.
            ESP_ERROR_CHECK(
                rmt_receive(
                    rx_channel,
                    raw_symbols,
                    sizeof(raw_symbols),
                    &receive_config
                )
            );
        }
    }
}
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define IR_RX_GPIO          GPIO_NUM_8
#define IR_TX_GPIO          GPIO_NUM_10
#define RMT_RESOLUTION_HZ   1000000
#define RX_SYMBOL_COUNT     512

#define WIFI_CONNECTED_BIT  BIT0

static const char *TAG = "ac_ir_http";

typedef enum {
    LEARN_NONE = -1,
    LEARN_ON = 0,
    LEARN_OFF = 1,
} learn_slot_t;

static rmt_channel_handle_t rx_channel;
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t copy_encoder;
static QueueHandle_t rx_queue;
static EventGroupHandle_t wifi_event_group;

static rmt_symbol_word_t rx_symbols[RX_SYMBOL_COUNT];
static rmt_symbol_word_t saved_frames[2][RX_SYMBOL_COUNT];
static size_t saved_counts[2] = {0, 0};

static volatile learn_slot_t learn_slot = LEARN_NONE;
static httpd_handle_t server = NULL;

static const char *nvs_keys[2] = {"ac_on", "ac_off"};

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

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi verbroken; opnieuw verbinden");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "ESP32 IP-adres: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    if (CONFIG_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Stel de Wi-Fi-SSID in via idf.py menuconfig");
        abort();
    }

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    if (strlcpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID,
                sizeof(wifi_config.sta.ssid)) >= sizeof(wifi_config.sta.ssid) ||
        strlcpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD,
                sizeof(wifi_config.sta.password)) >= sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "De geconfigureerde Wi-Fi-gegevens zijn te lang");
        abort();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Wachten op Wi-Fi...");
    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );
}

static void load_saved_frames(void)
{
    nvs_handle_t handle;
    esp_err_t open_err = nvs_open("ac_ir", NVS_READONLY, &handle);
    if (open_err == ESP_ERR_NVS_NOT_FOUND) {
        memset(saved_counts, 0, sizeof(saved_counts));
        ESP_LOGI(TAG, "Nog geen IR-opdrachten opgeslagen; leer eerst aan en uit in");
        return;
    }
    ESP_ERROR_CHECK(open_err);

    for (int slot = 0; slot < 2; slot++) {
        size_t bytes = sizeof(saved_frames[slot]);
        esp_err_t err = nvs_get_blob(handle, nvs_keys[slot],
                                     saved_frames[slot], &bytes);

        if (err == ESP_OK && bytes % sizeof(rmt_symbol_word_t) == 0) {
            saved_counts[slot] = bytes / sizeof(rmt_symbol_word_t);
            ESP_LOGI(TAG, "%s geladen: %u symbolen",
                     nvs_keys[slot], (unsigned)saved_counts[slot]);
        } else {
            saved_counts[slot] = 0;
            ESP_LOGI(TAG, "%s is nog niet opgeslagen", nvs_keys[slot]);
        }
    }

    nvs_close(handle);
}

static void save_frame(learn_slot_t slot, size_t count)
{
    if (slot != LEARN_ON && slot != LEARN_OFF) {
        return;
    }

    memcpy(saved_frames[slot], rx_symbols, count * sizeof(rmt_symbol_word_t));

    // De IR-ontvanger geeft de pulsen meestal geïnverteerd door.
    // Keer de niveaus om voor verzending via de IR-led.
    for (size_t i = 0; i < count; i++) {
        saved_frames[slot][i].level0 = !saved_frames[slot][i].level0;
        saved_frames[slot][i].level1 = !saved_frames[slot][i].level1;
    }

    saved_counts[slot] = count;

    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("ac_ir", NVS_READWRITE, &handle));
    ESP_ERROR_CHECK(nvs_set_blob(
        handle,
        nvs_keys[slot],
        saved_frames[slot],
        count * sizeof(rmt_symbol_word_t)
    ));
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);

    ESP_LOGI(TAG, "%s opgeslagen: %u symbolen",
             nvs_keys[slot], (unsigned)count);
}

static esp_err_t learn_handler(httpd_req_t *req)
{
    learn_slot_t requested_slot = (learn_slot_t)(intptr_t)req->user_ctx;

    learn_slot = requested_slot;

    const char *name = requested_slot == LEARN_ON ? "aan" : "uit";
    char response[100];
    snprintf(response, sizeof(response),
             "Klaar om de IR-opdracht voor %s op te nemen. Druk nu op de originele afstandsbediening.",
             name);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static esp_err_t send_handler(httpd_req_t *req)
{
    int slot = (int)(intptr_t)req->user_ctx;

    if (saved_counts[slot] == 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Geen opname opgeslagen. Gebruik eerst /learn/on of /learn/off.");
        return ESP_OK;
    }

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    ESP_ERROR_CHECK(rmt_transmit(
        tx_channel,
        copy_encoder,
        saved_frames[slot],
        saved_counts[slot] * sizeof(rmt_symbol_word_t),
        &transmit_config
    ));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(5000)));

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "IR-opdracht verzonden");
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char response[128];
    snprintf(response, sizeof(response),
             "{\"on_symbols\":%u,\"off_symbols\":%u}",
             (unsigned)saved_counts[LEARN_ON],
             (unsigned)saved_counts[LEARN_OFF]);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t learn_on = {
        .uri = "/learn/on",
        .method = HTTP_POST,
        .handler = learn_handler,
        .user_ctx = (void *)(intptr_t)LEARN_ON,
    };
    httpd_uri_t learn_off = {
        .uri = "/learn/off",
        .method = HTTP_POST,
        .handler = learn_handler,
        .user_ctx = (void *)(intptr_t)LEARN_OFF,
    };
    httpd_uri_t send_on = {
        .uri = "/api/ac/on",
        .method = HTTP_POST,
        .handler = send_handler,
        .user_ctx = (void *)(intptr_t)LEARN_ON,
    };
    httpd_uri_t send_off = {
        .uri = "/api/ac/off",
        .method = HTTP_POST,
        .handler = send_handler,
        .user_ctx = (void *)(intptr_t)LEARN_OFF,
    };
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &learn_on));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &learn_off));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &send_on));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &send_off));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));

    ESP_LOGI(TAG, "HTTP-server gestart");
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    load_saved_frames();

    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Kan de ontvangstruimte niet aanmaken");
        return;
    }

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
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(
        rx_channel, &callbacks, rx_queue));

    rmt_tx_channel_config_t tx_config = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));

    rmt_carrier_config_t carrier_config = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33,
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

    rmt_copy_encoder_config_t encoder_config = {0};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &copy_encoder));

    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 30000000,
    };

    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_ERROR_CHECK(rmt_enable(tx_channel));

    wifi_start();
    start_http_server();

    ESP_LOGI(TAG, "IR-ontvanger luistert; HTTP-bediening is actief");

    ESP_ERROR_CHECK(rmt_receive(
        rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config));

    while (1) {
        rmt_rx_done_event_data_t rx_data;

        if (xQueueReceive(rx_queue, &rx_data, portMAX_DELAY) == pdTRUE) {
            size_t count = rx_data.num_symbols;
            learn_slot_t slot = learn_slot;
            learn_slot = LEARN_NONE;

            if (count > 0 && count <= RX_SYMBOL_COUNT &&
                (slot == LEARN_ON || slot == LEARN_OFF)) {
                save_frame(slot, count);
            } else if (count > 0) {
                ESP_LOGI(TAG, "IR ontvangen (%u symbolen), niet opgeslagen",
                         (unsigned)count);
            }

            ESP_ERROR_CHECK(rmt_receive(
                rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config));
        }
    }
}

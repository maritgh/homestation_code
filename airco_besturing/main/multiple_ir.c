#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_presets.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define IR_RX_GPIO          GPIO_NUM_8
#define IR_TX_GPIO          GPIO_NUM_10

#define RMT_RESOLUTION_HZ   1000000
#define RX_SYMBOL_COUNT     512

#define WIFI_CONNECTED_BIT  BIT0
#define MAX_FRAME_BYTES     (RX_SYMBOL_COUNT * sizeof(rmt_symbol_word_t))

static const char *TAG = "ac_ir_http";

static rmt_channel_handle_t rx_channel;
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t copy_encoder;
static QueueHandle_t rx_queue;
static EventGroupHandle_t wifi_event_group;
static httpd_handle_t server = NULL;
static SemaphoreHandle_t frames_mutex;

static rmt_symbol_word_t rx_symbols[RX_SYMBOL_COUNT];
static rmt_symbol_word_t *saved_frames[AC_PRESET_COUNT];
static size_t saved_counts[AC_PRESET_COUNT] = {0};

static int learn_slot = -1;


/* Wordt aangeroepen wanneer een IR-opname klaar is. */
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


/* Wi-Fi-events: opnieuw verbinden na verlies van verbinding
   en het IP-adres tonen zodra verbinding is gemaakt. */
static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi verbroken; opnieuw verbinden");
        esp_wifi_connect();
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "ESP32 IP-adres: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* Verbindt de ESP32 met je Wi-Fi-netwerk. */
static void wifi_start(void)
{
    if (CONFIG_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Stel de Wi-Fi-SSID in via idf.py menuconfig");
        abort();
    }

    wifi_event_group = xEventGroupCreate();

    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Kan Wi-Fi-eventgroep niet aanmaken");
        abort();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL
    ));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL
    ));

    wifi_config_t wifi_config = {0};

    if (strlcpy(
        (char *)wifi_config.sta.ssid,
        CONFIG_WIFI_SSID,
        sizeof(wifi_config.sta.ssid)
    ) >= sizeof(wifi_config.sta.ssid)) {
        ESP_LOGE(TAG, "De geconfigureerde Wi-Fi-SSID is te lang");
        abort();
    }

    if (strlcpy(
        (char *)wifi_config.sta.password,
        CONFIG_WIFI_PASSWORD,
        sizeof(wifi_config.sta.password)
    ) >= sizeof(wifi_config.sta.password)) {
        ESP_LOGE(TAG, "Het geconfigureerde Wi-Fi-wachtwoord is te lang");
        abort();
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Wachten op verbinding met Wi-Fi...");

    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );
}


/* Zoekt een preset op basis van de id uit ac_presets.h. */
static int find_preset(const char *id)
{
    for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
        if (strcmp(id, AC_PRESETS[i].id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static void set_learn_slot(int slot)
{
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    learn_slot = slot;
    xSemaphoreGive(frames_mutex);
}

static int take_learn_slot(void)
{
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    int slot = learn_slot;
    learn_slot = -1;
    xSemaphoreGive(frames_mutex);
    return slot;
}


/* Haalt de preset-id uit de query van een HTTP-aanvraag,
   bijvoorbeeld /learn?id=koelen19auto. */
static bool get_preset_id_from_request(
    httpd_req_t *req,
    char *id,
    size_t id_size)
{
    char query[80];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }

    return httpd_query_key_value(
        query,
        "id",
        id,
        id_size
    ) == ESP_OK;
}


/* Laadt eerder opgenomen IR-presets uit NVS. */
static void load_saved_frames_from_partition(const char *partition)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open_from_partition(partition, "ac_ir", NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    ESP_ERROR_CHECK(err);

    for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
        size_t bytes = 0;
        err = nvs_get_blob(handle, AC_PRESETS[i].nvs_key, NULL, &bytes);

        if (err == ESP_OK &&
            bytes > 0 &&
            bytes <= MAX_FRAME_BYTES &&
            bytes % sizeof(rmt_symbol_word_t) == 0) {
            rmt_symbol_word_t *frame = malloc(bytes);
            if (frame == NULL) {
                ESP_LOGE(TAG, "Onvoldoende geheugen voor preset '%s'",
                         AC_PRESETS[i].id);
                continue;
            }

            size_t loaded_bytes = bytes;
            err = nvs_get_blob(handle, AC_PRESETS[i].nvs_key, frame,
                               &loaded_bytes);
            if (err != ESP_OK || loaded_bytes != bytes) {
                ESP_LOGW(TAG, "Preset '%s' kon niet volledig worden geladen",
                         AC_PRESETS[i].id);
                free(frame);
                continue;
            }

            free(saved_frames[i]);
            saved_frames[i] = frame;
            saved_counts[i] = bytes / sizeof(rmt_symbol_word_t);

            ESP_LOGI(
                TAG,
                "Preset '%s' geladen: %u symbolen",
                AC_PRESETS[i].id,
                (unsigned)saved_counts[i]
            );
        } else if (err == ESP_ERR_NVS_NOT_FOUND && saved_counts[i] == 0) {
            ESP_LOGI(
                TAG,
                "Preset '%s' is nog niet opgenomen",
                AC_PRESETS[i].id
            );
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Preset '%s' heeft een ongeldige opname",
                     AC_PRESETS[i].id);
        }
    }

    nvs_close(handle);
}


/* Slaat een ontvangen opname op onder de gekozen preset. */
static void save_frame(int slot, size_t count)
{
    if (slot < 0 ||
        slot >= AC_PRESET_COUNT ||
        count == 0 ||
        count > RX_SYMBOL_COUNT) {
        ESP_LOGE(TAG, "Ongeldige preset of IR-opname");
        return;
    }

    rmt_symbol_word_t *frame = malloc(count * sizeof(*frame));
    if (frame == NULL) {
        ESP_LOGE(TAG, "Onvoldoende geheugen om preset '%s' op te slaan",
                 AC_PRESETS[slot].id);
        return;
    }

    memcpy(frame, rx_symbols, count * sizeof(*frame));

    /*
     * De IR-ontvanger geeft de IR-pulsen meestal geïnverteerd door.
     * Keer de niveaus om voordat we het signaal via de IR-led versturen.
     */
    for (size_t i = 0; i < count; i++) {
        frame[i].level0 = !frame[i].level0;
        frame[i].level1 = !frame[i].level1;
    }

    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open_from_partition("ir_frames", "ac_ir", NVS_READWRITE, &handle));

    ESP_ERROR_CHECK(nvs_set_blob(
        handle,
        AC_PRESETS[slot].nvs_key,
        frame,
        count * sizeof(*frame)
    ));

    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);

    free(saved_frames[slot]);
    saved_frames[slot] = frame;
    saved_counts[slot] = count;
    xSemaphoreGive(frames_mutex);

    ESP_LOGI(
        TAG,
        "Preset '%s' opgeslagen: %u symbolen",
        AC_PRESETS[slot].id,
        (unsigned)count
    );
}


/* HTTP: /learn?id=koelen19auto
   De volgende ontvangen afstandsbedieningsopname wordt opgeslagen. */
static esp_err_t learn_handler(httpd_req_t *req)
{
    char id[32];

    if (!get_preset_id_from_request(req, id, sizeof(id))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(
            req,
            "Geef een preset-id mee, bijvoorbeeld ?id=koelen19auto"
        );
        return ESP_OK;
    }

    int slot = find_preset(id);

    if (slot < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Onbekende preset-id");
        return ESP_OK;
    }

    set_learn_slot(slot);

    char response[160];

    snprintf(
        response,
        sizeof(response),
        "Druk nu op de afstandsbediening voor preset: %s",
        AC_PRESETS[slot].description
    );

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, response);

    ESP_LOGI(TAG, "Opname gestart voor preset '%s'", id);

    return ESP_OK;
}


/* HTTP: /api/ac?id=koelen19auto
   Verstuurt de opgeslagen IR-opname. */
static esp_err_t send_handler(httpd_req_t *req)
{
    char id[32];

    if (!get_preset_id_from_request(req, id, sizeof(id))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(
            req,
            "Geef een preset-id mee, bijvoorbeeld ?id=koelen19auto"
        );
        return ESP_OK;
    }

    int slot = find_preset(id);

    if (slot < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "Onbekende preset-id");
        return ESP_OK;
    }

    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    if (saved_counts[slot] == 0) {
        xSemaphoreGive(frames_mutex);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(
            req,
            "Deze preset is nog niet opgenomen. Gebruik eerst /learn."
        );
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

    ESP_ERROR_CHECK(
        rmt_tx_wait_all_done(tx_channel, pdMS_TO_TICKS(5000))
    );

    xSemaphoreGive(frames_mutex);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "IR-opdracht verzonden");

    ESP_LOGI(TAG, "Preset '%s' verzonden", id);

    return ESP_OK;
}


/* HTTP: /status
   Toont hoeveel symbolen er per preset zijn opgeslagen. */
static esp_err_t status_handler(httpd_req_t *req)
{
    char response[AC_PRESET_COUNT * 64 + 3];
    size_t used = 0;
    xSemaphoreTake(frames_mutex, portMAX_DELAY);

    used += snprintf(response + used, sizeof(response) - used, "{");

    for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
        used += snprintf(
            response + used,
            sizeof(response) - used,
            "%s\"%s\":%u",
            (i == 0) ? "" : ",",
            AC_PRESETS[i].id,
            (unsigned)saved_counts[i]
        );

        if (used >= sizeof(response)) {
            break;
        }
    }

    if (used < sizeof(response)) {
        snprintf(response + used, sizeof(response) - used, "}");
    }

    xSemaphoreGive(frames_mutex);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response);

    return ESP_OK;
}


/* JSON v1: base64 van 32-bits RMT-symbolen in little-endian volgorde.
 * Bit 0..14: duur0, bit 15: niveau0, bit 16..30: duur1, bit 31: niveau1.
 * De niveaus zijn al geschikt voor verzending; niet opnieuw inverteren.
 */
static esp_err_t export_presets_handler(httpd_req_t *req)
{
    unsigned char *raw = malloc(MAX_FRAME_BYTES);
    char *encoded = malloc(4 * ((MAX_FRAME_BYTES + 2) / 3) + 1);
    if (raw == NULL || encoded == NULL) {
        free(raw);
        free(encoded);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Onvoldoende geheugen");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=ir_presets.json");
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    esp_err_t err = httpd_resp_sendstr_chunk(req,
        "{\"version\":1,\"resolution_hz\":1000000,\"carrier_hz\":38000,\"presets\":{");
    bool first = true;
    for (size_t slot = 0; slot < AC_PRESET_COUNT && err == ESP_OK; slot++) {
        size_t count = saved_counts[slot];
        if (count == 0 || saved_frames[slot] == NULL) {
            continue;
        }
        for (size_t i = 0; i < count; i++) {
            uint32_t value = saved_frames[slot][i].val;
            for (size_t b = 0; b < 4; b++) {
                raw[4 * i + b] = (unsigned char)(value >> (8 * b));
            }
        }
        size_t encoded_len;
        int result = mbedtls_base64_encode((unsigned char *)encoded,
            4 * ((MAX_FRAME_BYTES + 2) / 3) + 1,
            &encoded_len, raw, count * 4);
        if (result != 0) {
            err = ESP_FAIL;
            break;
        }
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "%s\"%s\":\"", first ? "" : ",", AC_PRESETS[slot].id);
        first = false;
        err = httpd_resp_sendstr_chunk(req, prefix);
        if (err == ESP_OK) err = httpd_resp_send_chunk(req, encoded, encoded_len);
        if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "\"");
    }
    xSemaphoreGive(frames_mutex);
    free(raw);
    free(encoded);
    if (err == ESP_OK) err = httpd_resp_sendstr_chunk(req, "}}");
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

/* Het hulpscript importeert één preset per aanvraag om RAM te sparen. */
static esp_err_t import_preset_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Verstuur een preset per aanvraag (maximaal 4096 bytes)");
    }
    char *body = malloc(req->content_len + 1);
    unsigned char *raw = malloc(MAX_FRAME_BYTES);
    if (body == NULL || raw == NULL) {
        free(body);
        free(raw);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Onvoldoende geheugen");
    }
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            free(body);
            free(raw);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Upload timeout");
            }
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(body, received + 1, NULL, true);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *resolution = cJSON_GetObjectItemCaseSensitive(root, "resolution_hz");
    cJSON *carrier = cJSON_GetObjectItemCaseSensitive(root, "carrier_hz");
    cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    cJSON *frame = cJSON_IsObject(presets) ? presets->child : NULL;
    int slot = frame && frame->string ? find_preset(frame->string) : -1;
    size_t bytes = 0;
    bool valid = cJSON_IsObject(root) && cJSON_IsNumber(version) && version->valuedouble == 1 &&
        cJSON_IsNumber(resolution) && resolution->valuedouble == RMT_RESOLUTION_HZ &&
        cJSON_IsNumber(carrier) && carrier->valuedouble == 38000 &&
        frame && frame->next == NULL && slot >= 0 && cJSON_IsString(frame);
    if (valid) {
        valid = mbedtls_base64_decode(raw, MAX_FRAME_BYTES, &bytes,
            (const unsigned char *)frame->valuestring, strlen(frame->valuestring)) == 0 &&
            bytes > 0 && bytes <= MAX_FRAME_BYTES &&
            bytes % sizeof(rmt_symbol_word_t) == 0;
    }
    cJSON_Delete(root);
    free(body);
    if (!valid) {
        free(raw);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Ongeldig presetbestand of onbekende preset");
    }

    // Zet de expliciete little-endian codering om naar lokale RMT-symbolen.
    rmt_symbol_word_t *symbols = (rmt_symbol_word_t *)raw;
    for (size_t i = 0; i < bytes / 4; i++) {
        uint32_t value = (uint32_t)raw[4 * i] | ((uint32_t)raw[4 * i + 1] << 8) |
            ((uint32_t)raw[4 * i + 2] << 16) | ((uint32_t)raw[4 * i + 3] << 24);
        symbols[i].val = value;
    }
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition("ir_frames", "ac_ir", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, AC_PRESETS[slot].nvs_key, symbols, bytes);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        free(saved_frames[slot]);
        saved_frames[slot] = symbols;
        saved_counts[slot] = bytes / sizeof(*symbols);
        raw = NULL;
        learn_slot = -1;
    }
    xSemaphoreGive(frames_mutex);
    free(raw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Import opslaan mislukt: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Kan preset niet opslaan");
    }
    return httpd_resp_sendstr(req, "Preset geimporteerd");
}


/* Start de HTTP-server en registreert de routes. */
static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(httpd_start(&server, &config));

    httpd_uri_t learn_route = {
        .uri = "/learn",
        .method = HTTP_POST,
        .handler = learn_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t send_route = {
        .uri = "/api/ac",
        .method = HTTP_POST,
        .handler = send_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t status_route = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t export_route = {
        .uri = "/presets/export", .method = HTTP_GET,
        .handler = export_presets_handler,
    };
    httpd_uri_t import_route = {
        .uri = "/presets/import", .method = HTTP_POST,
        .handler = import_preset_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &export_route));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &import_route));

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &learn_route));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &send_route));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_route));

    ESP_LOGI(TAG, "HTTP-server gestart");
}


void app_main(void)
{
    /*
     * Initialiseer NVS. NVS wordt gebruikt om de IR-opnames te bewaren,
     * ook nadat de ESP32 opnieuw is opgestart.
     */
    esp_err_t nvs_err = nvs_flash_init();

    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    ESP_ERROR_CHECK(nvs_flash_init_partition("ir_frames"));

    // Bestaande opnames blijven bruikbaar; nieuwe opnames hebben voorrang.
    load_saved_frames_from_partition("nvs");
    load_saved_frames_from_partition("ir_frames");

    frames_mutex = xSemaphoreCreateMutex();
    if (frames_mutex == NULL) {
        ESP_LOGE(TAG, "Kan de preset-lock niet aanmaken");
        return;
    }

    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Kan de IR-ontvangstqueue niet aanmaken");
        return;
    }

    /* RMT-ontvanger */
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
        rx_channel,
        &callbacks,
        rx_queue
    ));

    /* RMT-zender */
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_config, &tx_channel));

    /* IR-draaggolf */
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = 38000,
        .duty_cycle = 0.33,
    };

    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_config));

    /* Encoder voor het opnieuw verzenden van de ruwe symbolen */
    rmt_copy_encoder_config_t encoder_config = {0};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&encoder_config, &copy_encoder));

    /* De stilte na het signaal beëindigt de IR-opname */
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 30000000,
    };

    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_ERROR_CHECK(rmt_enable(tx_channel));

    wifi_start();
    start_http_server();

    ESP_LOGI(TAG, "IR-ontvanger luistert");
    ESP_LOGI(TAG, "Gebruik POST /learn?id=PRESET om een preset op te nemen");
    ESP_LOGI(TAG, "Gebruik POST /api/ac?id=PRESET om een preset te versturen");

    ESP_ERROR_CHECK(rmt_receive(
        rx_channel,
        rx_symbols,
        sizeof(rx_symbols),
        &receive_config
    ));

    while (1) {
        rmt_rx_done_event_data_t rx_data;

        if (xQueueReceive(rx_queue, &rx_data, portMAX_DELAY) == pdTRUE) {
            size_t count = rx_data.num_symbols;

            /*
             * Lees welke preset wordt opgenomen en zet de opname-instelling
             * daarna terug naar -1.
             */
            int slot = take_learn_slot();

            if (count > 0 &&
                count <= RX_SYMBOL_COUNT &&
                slot >= 0 &&
                slot < AC_PRESET_COUNT) {
                save_frame(slot, count);
            } else if (count > 0) {
                ESP_LOGI(
                    TAG,
                    "IR ontvangen (%u symbolen), niet opgeslagen",
                    (unsigned)count
                );
            }

            /* Start de ontvanger opnieuw voor de volgende opname. */
            ESP_ERROR_CHECK(rmt_receive(
                rx_channel,
                rx_symbols,
                sizeof(rx_symbols),
                &receive_config
            ));
        }
    }
}

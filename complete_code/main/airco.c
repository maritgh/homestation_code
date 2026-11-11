#include "airco.h"
#include "ac_presets.h"
#include "board_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define RMT_RESOLUTION_HZ 1000000
#define IR_CARRIER_HZ 38000
#define RX_SYMBOL_COUNT 512
#define MAX_FRAME_BYTES (RX_SYMBOL_COUNT * sizeof(rmt_symbol_word_t))
#define COMMAND_QUEUE_LENGTH 4
#define IR_RECEIVE_TASK_STACK_SIZE 3072
#define IR_RECEIVE_TASK_PRIORITY 2
#define IR_COMMAND_TASK_STACK_SIZE 6144
#define IR_COMMAND_TASK_PRIORITY 1
#define IR_STORAGE_PARTITION "ir_frames"
#define IR_STORAGE_NAMESPACE "ac_ir"
#define IR_FORMAT_VERSION 1
#define IR_SYMBOL_BYTES 4

_Static_assert(sizeof(rmt_symbol_word_t) == IR_SYMBOL_BYTES, "JSON v1 requires 4-byte RMT symbols");

static const char *TAG = "airco";
static rmt_channel_handle_t rx_channel;
static rmt_channel_handle_t tx_channel;
static rmt_encoder_handle_t copy_encoder;
static QueueHandle_t rx_queue;
static QueueHandle_t command_queue;
static SemaphoreHandle_t frames_mutex;
static bool ready;
static rmt_symbol_word_t rx_symbols[RX_SYMBOL_COUNT];
static rmt_symbol_word_t *saved_frames[AC_PRESET_COUNT];
static size_t saved_counts[AC_PRESET_COUNT];
static int learn_slot = -1;
static const rmt_receive_config_t receive_config = {
    .signal_range_min_ns = 2000,
    .signal_range_max_ns = 30000000,
};

typedef struct {
    airco_reply_fn reply;
    void *context;
    char json[];
} ir_command_t;

static bool ir_rx_done_callback(rmt_channel_handle_t channel,
                                const rmt_rx_done_event_data_t *event_data, void *user_data)
{
    (void)channel;
    BaseType_t task_woken = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_data, event_data, &task_woken);
    return task_woken == pdTRUE;
}

static int find_preset(const char *id)
{
    if (id) {
        for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
            if (strcmp(id, AC_PRESETS[i].id) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

/* Laadt eerder opgenomen IR-presets uit NVS. */
static esp_err_t load_saved_frames_from_partition(const char *partition)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open_from_partition(partition, IR_STORAGE_NAMESPACE, NVS_READONLY, &handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
        size_t bytes = 0;
        err = nvs_get_blob(handle, AC_PRESETS[i].nvs_key, NULL, &bytes);

        if (err == ESP_OK && bytes > 0 && bytes <= MAX_FRAME_BYTES &&
            bytes % sizeof(rmt_symbol_word_t) == 0) {
            rmt_symbol_word_t *frame = malloc(bytes);
            if (frame == NULL) {
                ESP_LOGE(TAG, "Onvoldoende geheugen voor preset '%s'", AC_PRESETS[i].id);
                continue;
            }

            size_t loaded_bytes = bytes;
            err = nvs_get_blob(handle, AC_PRESETS[i].nvs_key, frame, &loaded_bytes);
            if (err != ESP_OK || loaded_bytes != bytes) {
                ESP_LOGW(TAG, "Preset '%s' kon niet volledig worden geladen", AC_PRESETS[i].id);
                free(frame);
                continue;
            }

            free(saved_frames[i]);
            saved_frames[i] = frame;
            saved_counts[i] = bytes / sizeof(rmt_symbol_word_t);

            ESP_LOGI(TAG, "Preset '%s' geladen: %u symbolen", AC_PRESETS[i].id,
                     (unsigned)saved_counts[i]);
        } else if (err == ESP_ERR_NVS_NOT_FOUND && saved_counts[i] == 0) {
            ESP_LOGI(TAG, "Preset '%s' is nog niet opgenomen", AC_PRESETS[i].id);
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Preset '%s' heeft een ongeldige opname", AC_PRESETS[i].id);
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

/* Neemt frame alleen over als zowel de permanente opslag als de RAM-update slagen. */
static esp_err_t store_frame(int slot, rmt_symbol_word_t *frame, size_t count)
{
    nvs_handle_t handle;
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    esp_err_t err =
        nvs_open_from_partition(IR_STORAGE_PARTITION, IR_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, AC_PRESETS[slot].nvs_key, frame, count * sizeof(*frame));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        free(saved_frames[slot]);
        saved_frames[slot] = frame;
        saved_counts[slot] = count;
    }
    xSemaphoreGive(frames_mutex);
    return err;
}

static void receive_task(void *arg)
{
    (void)arg;
    rmt_rx_done_event_data_t data;
    while (xQueueReceive(rx_queue, &data, portMAX_DELAY) == pdTRUE) {
        xSemaphoreTake(frames_mutex, portMAX_DELAY);
        int slot = learn_slot;
        learn_slot = -1;
        xSemaphoreGive(frames_mutex);
        size_t count = data.num_symbols;
        if (slot >= 0 && count > 0 && count <= RX_SYMBOL_COUNT) {
            rmt_symbol_word_t *frame = malloc(count * sizeof(*frame));
            esp_err_t err = ESP_ERR_NO_MEM;
            if (frame) {
                memcpy(frame, rx_symbols, count * sizeof(*frame));
                // De niveaus van de gedemoduleerde ontvanger zijn omgekeerd ten opzichte van de led.
                for (size_t i = 0; i < count; i++) {
                    frame[i].level0 = !frame[i].level0;
                    frame[i].level1 = !frame[i].level1;
                }
                err = store_frame(slot, frame, count);
                if (err != ESP_OK) {
                    free(frame);
                }
            }
            ESP_LOGI(TAG, "Leer '%s' (%u symbols): %s", AC_PRESETS[slot].id, (unsigned)count,
                     esp_err_to_name(err));
        }
        esp_err_t err = rmt_receive(rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Herstart IR ontvanger: %s", esp_err_to_name(err));
            break;
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t send_frame(int slot)
{
    // Gebruik een aparte kopie zodat aanleren of importeren een actieve zendbuffer niet kan wijzigen.
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    size_t bytes = saved_counts[slot] * sizeof(rmt_symbol_word_t);
    rmt_symbol_word_t *frame = bytes ? malloc(bytes) : NULL;
    if (frame) {
        memcpy(frame, saved_frames[slot], bytes);
    }
    xSemaphoreGive(frames_mutex);
    if (!bytes) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!frame) {
        return ESP_ERR_NO_MEM;
    }
    rmt_transmit_config_t config = {.loop_count = 0};
    esp_err_t err = rmt_transmit(tx_channel, copy_encoder, frame, bytes, &config);
    if (err == ESP_OK) {
        // De maximale frameduur is ongeveer 34 seconden (512 * 2 * 32767 us).
        err = rmt_tx_wait_all_done(tx_channel, -1);
    }
    free(frame);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Preset '%s' gestuurd", AC_PRESETS[slot].id);
    }
    return err;
}

static void reply_json(const ir_command_t *command, cJSON *response)
{
    if (!response) {
        return;
    }
    char *json = cJSON_PrintUnformatted(response);
    if (json) {
        if (command->reply) {
            command->reply(json, command->context);
        }
        free(json);
    }
    cJSON_Delete(response);
}

static cJSON *make_response(const char *action, const char *id, esp_err_t err)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return NULL;
    }
    cJSON_AddStringToObject(response, "action", action ? action : "invalid");
    if (id) {
        cJSON_AddStringToObject(response, "id", id);
    }
    cJSON_AddBoolToObject(response, "ok", err == ESP_OK);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(response, "error", esp_err_to_name(err));
    }
    return response;
}

/* JSON v1 behoudt het bestaande formaat met base64 en little-endian RMT-symbolen.
 * Geïmporteerde niveaus beschrijven al de led-uitgang en mogen niet worden omgekeerd. */
static esp_err_t import_preset(const cJSON *root)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *resolution = cJSON_GetObjectItemCaseSensitive(root, "resolution_hz");
    const cJSON *carrier = cJSON_GetObjectItemCaseSensitive(root, "carrier_hz");
    const cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    const cJSON *entry = cJSON_IsObject(presets) ? presets->child : NULL;
    int slot = entry ? find_preset(entry->string) : -1;
    if (!cJSON_IsNumber(version) || version->valuedouble != IR_FORMAT_VERSION ||
        !cJSON_IsNumber(resolution) || resolution->valuedouble != RMT_RESOLUTION_HZ ||
        !cJSON_IsNumber(carrier) || carrier->valuedouble != IR_CARRIER_HZ || slot < 0 ||
        entry->next || !cJSON_IsString(entry)) {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned char *raw = malloc(MAX_FRAME_BYTES);
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }
    size_t bytes = 0;
    if (mbedtls_base64_decode(raw, MAX_FRAME_BYTES, &bytes,
                              (const unsigned char *)entry->valuestring,
                              strlen(entry->valuestring)) != 0 ||
        !bytes || bytes % sizeof(rmt_symbol_word_t) != 0) {
        free(raw);
        return ESP_ERR_INVALID_ARG;
    }
    rmt_symbol_word_t *symbols = (rmt_symbol_word_t *)raw;
    for (size_t i = 0; i < bytes / IR_SYMBOL_BYTES; i++) {
        const unsigned char *symbol = raw + i * IR_SYMBOL_BYTES;
        symbols[i].val = (uint32_t)symbol[0] | ((uint32_t)symbol[1] << 8) |
                         ((uint32_t)symbol[2] << 16) | ((uint32_t)symbol[3] << 24);
    }
    esp_err_t err = store_frame(slot, symbols, bytes / sizeof(*symbols));
    if (err != ESP_OK) {
        free(raw);
    } else {
        xSemaphoreTake(frames_mutex, portMAX_DELAY);
        learn_slot = -1;
        xSemaphoreGive(frames_mutex);
    }
    return err;
}

/* Eén antwoord per opgenomen preset voorkomt één grote export in het RAM. */
static esp_err_t export_presets(const ir_command_t *command)
{
    unsigned char *raw = malloc(MAX_FRAME_BYTES);
    size_t capacity = 4 * ((MAX_FRAME_BYTES + 2) / 3) + 1;
    unsigned char *encoded = malloc(capacity);
    if (!raw || !encoded) {
        free(raw);
        free(encoded);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (size_t slot = 0; slot < AC_PRESET_COUNT; slot++) {
        xSemaphoreTake(frames_mutex, portMAX_DELAY);
        size_t count = saved_counts[slot];
        for (size_t i = 0; i < count; i++) {
            uint32_t value = saved_frames[slot][i].val;
            for (size_t b = 0; b < IR_SYMBOL_BYTES; b++) {
                raw[IR_SYMBOL_BYTES * i + b] = (unsigned char)(value >> (8 * b));
            }
        }
        xSemaphoreGive(frames_mutex);
        if (!count) {
            continue;
        }
        size_t encoded_len;
        if (mbedtls_base64_encode(encoded, capacity, &encoded_len, raw, count * IR_SYMBOL_BYTES) !=
            0) {
            err = ESP_FAIL;
            break;
        }
        encoded[encoded_len] = '\0';
        cJSON *response = make_response("export", AC_PRESETS[slot].id, ESP_OK);
        if (!response) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddNumberToObject(response, "version", IR_FORMAT_VERSION);
        cJSON_AddNumberToObject(response, "resolution_hz", RMT_RESOLUTION_HZ);
        cJSON_AddNumberToObject(response, "carrier_hz", IR_CARRIER_HZ);
        cJSON *presets = cJSON_AddObjectToObject(response, "presets");
        cJSON_AddStringToObject(presets, AC_PRESETS[slot].id, (const char *)encoded);
        reply_json(command, response);
    }
    free(raw);
    free(encoded);
    return err;
}

static esp_err_t learn_preset(int slot)
{
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    learn_slot = slot;
    xSemaphoreGive(frames_mutex);
    return ESP_OK;
}

static cJSON *make_status_response(void)
{
    cJSON *response = make_response("status", NULL, ESP_OK);
    cJSON *presets = cJSON_AddObjectToObject(response, "presets");
    xSemaphoreTake(frames_mutex, portMAX_DELAY);
    for (size_t i = 0; i < AC_PRESET_COUNT; i++) {
        cJSON_AddNumberToObject(presets, AC_PRESETS[i].id, saved_counts[i]);
    }
    if (learn_slot >= 0) {
        cJSON_AddStringToObject(response, "learning", AC_PRESETS[learn_slot].id);
    }
    xSemaphoreGive(frames_mutex);
    return response;
}

static void process_command(const ir_command_t *command)
{
    cJSON *root = cJSON_ParseWithLengthOpts(command->json, strlen(command->json) + 1, NULL, true);
    const cJSON *action_item = cJSON_GetObjectItemCaseSensitive(root, "action");
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *action = cJSON_GetStringValue(action_item);
    const char *id = cJSON_GetStringValue(id_item);
    // Normale actuatorberichten gebruiken {"ir":"koelen19auto"}.
    if (!action_item) {
        action = "send";
        id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "ir"));
    }
    int slot = find_preset(id);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    cJSON *response = NULL;
    if (cJSON_IsObject(root) && action) {
        if (strcmp(action, "send") == 0 || strcmp(action, "learn") == 0) {
            err = slot < 0 ? ESP_ERR_NOT_FOUND : ESP_OK;
            if (err == ESP_OK) {
                err = strcmp(action, "send") == 0 ? send_frame(slot) : learn_preset(slot);
            }
        } else if (strcmp(action, "status") == 0) {
            err = ESP_OK;
            response = make_status_response();
        } else if (strcmp(action, "import") == 0) {
            err = import_preset(root);
        } else if (strcmp(action, "export") == 0) {
            err = export_presets(command);
            response = make_response(action, NULL, err);
            cJSON_AddBoolToObject(response, "complete", true);
        }
    }
    if (!response) {
        response = make_response(action, id, err);
    }
    if (err == ESP_OK && action && strcmp(action, "learn") == 0) {
        cJSON_AddStringToObject(response, "state", "waiting_for_ir");
    }
    reply_json(command, response);
    cJSON_Delete(root);
}

static void command_task(void *arg)
{
    (void)arg;
    ir_command_t *command;
    while (xQueueReceive(command_queue, &command, portMAX_DELAY) == pdTRUE) {
        process_command(command);
        free(command);
    }
    vTaskDelete(NULL);
}

esp_err_t airco_submit_json(const char *json, size_t length, airco_reply_fn reply,
                                  void *context)
{
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!json || !length || length > AIRCO_MAX_JSON_BYTES || memchr(json, '\0', length)) {
        return ESP_ERR_INVALID_ARG;
    }
    ir_command_t *command = malloc(sizeof(*command) + length + 1);
    if (!command) {
        return ESP_ERR_NO_MEM;
    }
    command->reply = reply;
    command->context = context;
    memcpy(command->json, json, length);
    command->json[length] = '\0';
    if (xQueueSend(command_queue, &command, 0) != pdTRUE) {
        free(command);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static bool valid_pins(void)
{
    const int reserved[] = {
        BOARD_I2C_SDA_GPIO,        BOARD_I2C_SCL_GPIO,        BOARD_DS18B20_GPIO,
        WIND_1_DATA_GPIO,        WIND_1_CLOCK_GPIO,        WIND_2_DATA_GPIO,
        WIND_2_CLOCK_GPIO,
    };
    if (!GPIO_IS_VALID_GPIO(IR_RX_GPIO) || !GPIO_IS_VALID_OUTPUT_GPIO(IR_TX_GPIO) ||
        IR_RX_GPIO == IR_TX_GPIO) {
        return false;
    }
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (IR_RX_GPIO == reserved[i] || IR_TX_GPIO == reserved[i]) {
            return false;
        }
    }
    return true;
}

esp_err_t airco_start(void)
{
    if (ready || frames_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_pins()) {
        ESP_LOGE(TAG, "IR GPIO heeft een conflict met een ander sensor check board_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "Initialaseer NVS zonder de presets te verwijderen");
    ESP_RETURN_ON_ERROR(nvs_flash_init_partition(IR_STORAGE_PARTITION), TAG,
                        "Initialiseer IR opslag");
    ESP_RETURN_ON_ERROR(load_saved_frames_from_partition("nvs"), TAG, "Laad legacy presets");
    ESP_RETURN_ON_ERROR(load_saved_frames_from_partition(IR_STORAGE_PARTITION), TAG,
                        "Laad presets");
    frames_mutex = xSemaphoreCreateMutex();
    rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    command_queue = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(ir_command_t *));
    if (!frames_mutex || !rx_queue || !command_queue) {
        return ESP_ERR_NO_MEM;
    }
    rmt_rx_channel_config_t rx_config = {
        .gpio_num = IR_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 256,
        .flags.with_dma = true,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_config, &rx_channel), TAG, "Create IR RX");
    rmt_rx_event_callbacks_t callbacks = {.on_recv_done = ir_rx_done_callback};
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(rx_channel, &callbacks, rx_queue), TAG,
                        "RX callback");
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = IR_TX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &tx_channel), TAG, "Create IR TX");
    rmt_carrier_config_t carrier_config = {.frequency_hz = IR_CARRIER_HZ, .duty_cycle = 0.33};
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(tx_channel, &carrier_config), TAG, "Set IR carrier");
    rmt_copy_encoder_config_t encoder_config = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&encoder_config, &copy_encoder), TAG,
                        "Create encoder");
    ESP_RETURN_ON_ERROR(rmt_enable(rx_channel), TAG, "Enable IR RX");
    ESP_RETURN_ON_ERROR(rmt_enable(tx_channel), TAG, "Enable IR TX");
    ESP_RETURN_ON_ERROR(rmt_receive(rx_channel, rx_symbols, sizeof(rx_symbols), &receive_config),
                        TAG, "Start IR RX");
    if (xTaskCreate(receive_task, "ir_receive", IR_RECEIVE_TASK_STACK_SIZE, NULL,
                    IR_RECEIVE_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(command_task, "ir_commands", IR_COMMAND_TASK_STACK_SIZE, NULL,
                    IR_COMMAND_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ready = true;
    ESP_LOGI(TAG, "IR klaar: RX GPIO %d, TX GPIO %d, carrier %d Hz", IR_RX_GPIO, IR_TX_GPIO,
             IR_CARRIER_HZ);
    return ESP_OK;
}

#include "mqtt.h"
#include "mqtt_config.h"
#include "airco.h"
#include "sensors.h"
#include "wind.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define MQTT_CONNECTED_BIT BIT1
#define CONNECTED_BITS (WIFI_CONNECTED_BIT | MQTT_CONNECTED_BIT)
#define WIFI_RETRY_US 5000000
#define MQTT_RECONNECT_TIMEOUT_MS 5000
#define MQTT_TASK_STACK_SIZE 4096
#define MQTT_TASK_PRIORITY 1

static const char *TAG = "homestation_mqtt";
static EventGroupHandle_t connection_events;
static esp_timer_handle_t wifi_retry_timer;
static esp_mqtt_client_handle_t client;
static char actuator_topic[128];
static char actuator_status_topic[144];
// Only the MQTT event handler owns this in-progress message.
static struct {
    char *data;
    size_t received;
    size_t total;
} actuator_message;

static void reset_actuator_payload(void)
{
    free(actuator_message.data);
    actuator_message.data = NULL;
    actuator_message.received = 0;
    actuator_message.total = 0;
}

static void publish_actuator_reply(const char *json, void *context)
{
    (void)context;
    // This callback runs in the IR worker, never in the MQTT event handler.
    if ((xEventGroupGetBits(connection_events) & CONNECTED_BITS) == CONNECTED_BITS &&
        esp_mqtt_client_publish(client, actuator_status_topic, json, 0, 0, 0) < 0) {
        ESP_LOGW(TAG, "Could not publish IR response");
    }
}

static void receive_actuator_data(const esp_mqtt_event_t *event)
{
    if (event->current_data_offset == 0) {
        reset_actuator_payload();
        if (!event->topic || event->topic_len != strlen(actuator_topic) ||
            memcmp(event->topic, actuator_topic, event->topic_len) != 0) {
            return;
        }
        if (event->total_data_len <= 0 || event->total_data_len > AIRCO_MAX_JSON_BYTES) {
            ESP_LOGW(TAG, "Invalid actuator payload size (maximum %d bytes)",
                     AIRCO_MAX_JSON_BYTES);
            return;
        }
        actuator_message.total = event->total_data_len;
        actuator_message.data = malloc(actuator_message.total + 1);
        if (!actuator_message.data) {
            reset_actuator_payload();
            ESP_LOGW(TAG, "No memory for actuator command");
            return;
        }
    }
    if (!actuator_message.data) {
        return;
    }
    // MQTT payloads can span multiple DATA events; topic is absent on later chunks.
    if (event->current_data_offset < 0 || event->data_len <= 0 || !event->data ||
        event->total_data_len != actuator_message.total ||
        event->current_data_offset != actuator_message.received ||
        (size_t)event->data_len > actuator_message.total - actuator_message.received) {
        ESP_LOGW(TAG, "Incomplete or inconsistent actuator payload");
        reset_actuator_payload();
        return;
    }
    memcpy(actuator_message.data + actuator_message.received, event->data, event->data_len);
    actuator_message.received += event->data_len;
    if (actuator_message.received == actuator_message.total) {
        actuator_message.data[actuator_message.total] = '\0';
        esp_err_t err = airco_submit_json(actuator_message.data, actuator_message.total,
                                                publish_actuator_reply, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Queue IR command: %s", esp_err_to_name(err));
        }
        reset_actuator_payload();
    }
}

static void wifi_connect(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(err));
        esp_timer_start_once(wifi_retry_timer, WIFI_RETRY_US);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_connect(NULL);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(connection_events, CONNECTED_BITS);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying in 5 seconds");
        esp_timer_stop(wifi_retry_timer);
        esp_timer_start_once(wifi_retry_timer, WIFI_RETRY_US);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_timer_stop(wifi_retry_timer);
        xEventGroupSetBits(connection_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected, IP address received");
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        xEventGroupSetBits(connection_events, MQTT_CONNECTED_BIT);
        ESP_LOGI(TAG, "MQTT connected");
        reset_actuator_payload();
        // Resubscribe after every reconnect (the broker uses a clean session).
        if (esp_mqtt_client_subscribe(client, actuator_topic, 0) < 0) {
            ESP_LOGE(TAG, "Could not subscribe to %s", actuator_topic);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        xEventGroupClearBits(connection_events, MQTT_CONNECTED_BIT);
        reset_actuator_payload();
        ESP_LOGW(TAG, "MQTT disconnected; automatic reconnect enabled");
        break;
    case MQTT_EVENT_DATA:
        receive_actuator_data((esp_mqtt_event_handle_t)data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT connection error; check broker address and transport");
        break;
    default:
        break;
    }
}

static void publish_value(const char *metric, float value)
{
    if (!isfinite(value) ||
        (xEventGroupGetBits(connection_events) & CONNECTED_BITS) != CONNECTED_BITS) {
        return;
    }

    char topic[128];
    char payload[192];
    int topic_len = snprintf(topic, sizeof(topic), "homestation/%s/%s", HOME_STATION_ID, metric);
    int payload_len = snprintf(payload, sizeof(payload), "{\"apiKey\":\"%s\",\"value\":%.2f}",
                               HOME_STATION_API_KEY, (double)value);
    if (topic_len < 0 || topic_len >= sizeof(topic) || payload_len < 0 ||
        payload_len >= sizeof(payload)) {
        ESP_LOGE(TAG, "MQTT topic or payload too long");
        return;
    }

    // Network delays never block sensor/display updates.
    if (esp_mqtt_client_enqueue(client, topic, payload, payload_len, 0, 0, true) < 0) {
        ESP_LOGW(TAG, "Could not queue %s", metric);
    }
}

static void publish_readings(void)
{
    wind_readings_t wind;
    if (wind_get_latest(&wind) && wind.valid) {
        publish_value("windSpeed", wind.speed_ms);
        
        ESP_LOGI(TAG, "windspeed %.1f m/s", (double)wind.speed_ms);
        publish_value("custom/windrichting", wind.direction_deg);
    }

    sensor_readings_t readings;
    if (!sensors_get_latest(&readings)) {
        return;
    }

    if (readings.climate_valid) {
        publish_value("temperature", readings.temperature_c);
        if (readings.humidity_percent >= 0 && readings.humidity_percent <= 100) {
            publish_value("humidity", readings.humidity_percent);
        }
    }
    if (readings.distance_valid) {
        publish_value("custom/distance", readings.distance_mm);
    }
    if (readings.water_temperature_valid) {
        publish_value("custom/watertemperatuur", readings.water_temperature_c);
    }
}

static void mqtt_task(void *arg)
{
    (void)arg;
    xEventGroupWaitBits(connection_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_err_t err = esp_mqtt_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        xEventGroupWaitBits(connection_events, CONNECTED_BITS, pdFALSE, pdTRUE, portMAX_DELAY);
        publish_readings();
        vTaskDelay(pdMS_TO_TICKS(HOME_STATION_PUBLISH_INTERVAL_MS));
    }
}

static esp_err_t configure_wifi(void)
{
    // Leave existing NVS data intact if initialization fails.
    ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "Initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Initialize network stack");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Create event loop");
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }

    connection_events = xEventGroupCreate();
    if (!connection_events) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = wifi_connect,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &wifi_retry_timer), TAG,
                        "Create retry timer");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "Initialize Wi-Fi");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Set Wi-Fi storage");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL),
                        TAG, "Register Wi-Fi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL),
                        TAG, "Register IP events");

    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = HOME_STATION_WIFI_SSID,
                .password = HOME_STATION_WIFI_PASSWORD,
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {.capable = true, .required = false},
            },
    };
    if (strlen(HOME_STATION_WIFI_PASSWORD) < 8) {
        ESP_LOGW(TAG, "Configured Wi-Fi password is shorter than the WPA2 minimum of 8 characters");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Set station mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Configure Wi-Fi");

    return ESP_OK;
}

static esp_err_t configure_mqtt(void)
{
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "Read station MAC");
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "hs-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = HOME_STATION_MQTT_URI,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = client_id,
        .network.reconnect_timeout_ms = MQTT_RECONNECT_TIMEOUT_MS,
        .buffer.size = 512,
        .outbox.limit = 4096,
    };
    client = esp_mqtt_client_init(&mqtt_config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event, NULL),
                        TAG, "Register MQTT events");
    return ESP_OK;
}

esp_err_t home_station_mqtt_start(void)
{
    int topic_len = snprintf(actuator_topic, sizeof(actuator_topic), "homestation/%s/actuator",
                             HOME_STATION_ID);
    int status_len =
        snprintf(actuator_status_topic, sizeof(actuator_status_topic), "%s/status", actuator_topic);
    if (topic_len < 0 || topic_len >= sizeof(actuator_topic) || status_len < 0 ||
        status_len >= sizeof(actuator_status_topic)) {
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_RETURN_ON_ERROR(configure_wifi(), TAG, "Configure Wi-Fi");
    ESP_RETURN_ON_ERROR(configure_mqtt(), TAG, "Configure MQTT");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Start Wi-Fi");
    if (xTaskCreate(mqtt_task, "homestation_mqtt", MQTT_TASK_STACK_SIZE, NULL, MQTT_TASK_PRIORITY,
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

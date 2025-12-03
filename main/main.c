#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "dht22.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "time.h"
#include "sys/time.h"
#include "esp_sntp.h"
#include "esp_crt_bundle.h"

#define WIFI_SSID "Steren COM-825"
#define WIFI_PASS "123456789"
#define DHT_GPIO GPIO_NUM_4

static const char *TAG = "DHT22_APP";
static bool wifi_connected = false;

// ⏱️ Intervalo dinámico (default 3 min)
static int update_interval_ms = 180000;

// ======================== BUFFER HTTP GLOBAL ========================

static char http_buffer[512];
static int http_buffer_len = 0;

// ======================== HTTP EVENT HANDLER ========================

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (http_buffer_len + evt->data_len < sizeof(http_buffer)) {
            memcpy(http_buffer + http_buffer_len, evt->data, evt->data_len);
            http_buffer_len += evt->data_len;
        }
    }
    return ESP_OK;
}

// ======================== WIFI ========================

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi desconectado, reconectando...");
        esp_wifi_connect();
        wifi_connected = false;
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        ESP_LOGI(TAG, "✅ WiFi conectado");
    }
}

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    esp_wifi_start();

    esp_wifi_connect();
}

// ======================== SNTP ========================

void init_sntp_and_timezone(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "mx.pool.ntp.org");
    sntp_init();

    setenv("TZ", "CST6CDT", 1);
    tzset();
}

// ======================== API UPDATE ========================

void get_update_interval_from_server()
{
    http_buffer_len = 0;
    memset(http_buffer, 0, sizeof(http_buffer));

    esp_http_client_config_t config = {
        .url = "https://damn-back.onrender.com/api/update",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {

        ESP_LOGI(TAG, "✅ HTTP Status = %d", esp_http_client_get_status_code(client));

        if (http_buffer_len > 0) {
            http_buffer[http_buffer_len] = '\0';

            ESP_LOGI(TAG, "✅ JSON recibido: %s", http_buffer);

            char *ms_ptr = strstr(http_buffer, "\"update_interval_ms\"");
            if (ms_ptr != NULL) {
                int new_interval = 0;
                sscanf(ms_ptr, "\"update_interval_ms\":%d", &new_interval);

                if (new_interval >= 4000 && new_interval <= 300000) {
                    update_interval_ms = new_interval;
                    ESP_LOGI(TAG, "✅ Intervalo actualizado: %d ms", update_interval_ms);
                } else {
                    ESP_LOGW(TAG, "⚠️ Intervalo fuera de rango");
                }
            } else {
                ESP_LOGW(TAG, "⚠️ update_interval_ms no encontrado");
            }

        } else {
            ESP_LOGW(TAG, "⚠️ Body vacío");
        }

    } else {
        ESP_LOGE(TAG, "❌ Error HTTP: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

// ======================== ENVÍO A BACKEND ========================

void send_to_server(float temp, float hum)
{
    time_t now;
    struct tm timeinfo;
    char timestamp[32];

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M", &timeinfo);

    char post_data[256];
    snprintf(post_data, sizeof(post_data),
             "{\"temperature\":%.1f,\"humidity\":%.1f,\"timestamp\":\"%s\"}",
             temp, hum, timestamp);

    esp_http_client_config_t config = {
        .url = "https://damn-back.onrender.com/api/telemetry",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Datos enviados correctamente");
    } else {
        ESP_LOGE(TAG, "❌ Error enviando datos");
    }

    esp_http_client_cleanup(client);
}

// ======================== MAIN ========================

void app_main(void)
{
    nvs_flash_init();
    wifi_init();

    while (!wifi_connected) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    init_sntp_and_timezone();
    dht22_init(DHT_GPIO);

    // ⏱️ Primer tiempo dinámico
    get_update_interval_from_server();

    while (1) {
        float temp = 0, hum = 0;

        if (dht22_read(&temp, &hum) == 0) {
            ESP_LOGI(TAG, "🌡️ Temp: %.1f°C  💧 Hum: %.1f%%", temp, hum);
            send_to_server(temp, hum);
        } else {
            ESP_LOGE(TAG, "❌ Error leyendo DHT22");
        }

        // 🔁 Pedir nuevo intervalo cada ciclo
        get_update_interval_from_server();

        ESP_LOGI(TAG, "⏳ Esperando %d ms...", update_interval_ms);
        vTaskDelay(pdMS_TO_TICKS(update_interval_ms));
    }
}

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

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi desconectado, reconectando...");
        esp_wifi_connect();
        wifi_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        ESP_LOGI(TAG, "WiFi conectado");
    }
}

void wifi_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &instance_any_id);

    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &instance_got_ip);

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
}

void init_sntp_and_timezone(void)
{
    ESP_LOGI(TAG, "Inicializando SNTP...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "mx.pool.ntp.org");
    sntp_init();
    setenv("TZ", "CST6CDT", 1);
    tzset();

    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;
    while (timeinfo.tm_year < (2020 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Esperando sincronización SNTP (%d/%d)...", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_year >= (2020 - 1900)) {
        char strftime_buf[64];
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        ESP_LOGI(TAG, "SNTP sincronizado: %s", strftime_buf);
    } else {
        ESP_LOGW(TAG, "No se pudo sincronizar la hora via SNTP (se usará reloj local)");
    }
}

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
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Datos enviados (status=%d) -> %s", status, post_data);

        char response_buf[256] = {0};
        int content_length = esp_http_client_fetch_headers(client);
        if (content_length > 0) {
            int read = esp_http_client_read_response(client, response_buf, sizeof(response_buf)-1);
            if (read >= 0) {
                response_buf[read] = 0;
                ESP_LOGI(TAG, "Respuesta backend: %s", response_buf);
            }
        } else {
            int read = esp_http_client_read_response(client, response_buf, sizeof(response_buf)-1);
            if (read >= 0) {
                response_buf[read] = 0;
                ESP_LOGI(TAG, "Respuesta backend: %s", response_buf);
            } else {
                ESP_LOGI(TAG, "No hay body en la respuesta");
            }
        }
    } else {
        ESP_LOGE(TAG, "Error enviando datos: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inicializando NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "Inicializando WiFi...");
    wifi_init();

    while (!wifi_connected) {
        ESP_LOGI(TAG, "Esperando conexión WiFi...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    init_sntp_and_timezone();

    // Inicializar DHT22
    dht22_init(DHT_GPIO);
    ESP_LOGI(TAG, "DHT22 inicializado");

    while (1) {
        float temp = 0, hum = 0;

        if (dht22_read(&temp, &hum) == 0) {
            ESP_LOGI(TAG, "Temp: %.1f°C   Hum: %.1f%%", temp, hum);
            send_to_server(temp, hum);
        } else {
            ESP_LOGE(TAG, "Error leyendo DHT22");
        }

        vTaskDelay(pdMS_TO_TICKS(180000));
    }
}

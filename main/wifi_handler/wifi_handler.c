#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "shared.h"
#include "cJSON.h"
#include <ctype.h> 

#define EXAMPLE_ESP_WIFI_SSID      "ESP32-AP"
#define EXAMPLE_ESP_WIFI_PASS      "12345678"
#define EXAMPLE_MAX_STA_CONN       4

#define TARGET_SSID "Tina"
#define TARGET_PASSWORD "Lunaekuche"

#define NTP_SERVER "pool.ntp.org"

#define TAG "WIFI_HANDLER"

#define MAX_HTTP_BUFFER 2048  // Buffer for full JSON response

static char http_buffer[MAX_HTTP_BUFFER];  // Store full JSON response
static int http_buffer_len = 0;

extern const uint8_t cert[] asm("_binary_open_meteo_crt_start");

void url_decode_inplace(char *str, size_t max_len) {
    char *p = str;
    char *q = str;
    
    while (*p && (q - str) < (int)max_len - 1) {
        if (*p == '%') {
            // Cast to unsigned char to fix signed char issue
            unsigned char c1 = (unsigned char)*(p + 1);
            unsigned char c2 = (unsigned char)*(p + 2);
            
            if (isxdigit(c1) && isxdigit(c2)) {
                char hex[3] = {(char)c1, (char)c2, '\0'};
                *q++ = (char)strtol(hex, NULL, 16);
                p += 3;
            } else {
                *q++ = *p++;
            }
        } else {
            *q++ = *p++;
        }
    }
    *q = '\0';
}

void wifi_retry_task(void *params) {
    while (1) {
        int max_retries = 10;
        for (retry_wifi_num = 0; retry_wifi_num < max_retries; retry_wifi_num++) {
            ESP_LOGI(TAG, "Retry %d/%d", retry_wifi_num+1, max_retries);
            esp_wifi_connect();
        
            // Wait max 15 seconds for success/disconnect
            uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(15000));
        
            if (wifi_connected) {
                ESP_LOGI(TAG, "Connected successfully!");
                break;  // Success - exit retry loop
            } else if (notified == 0) {
                ESP_LOGW(TAG, "Timeout - AP not responding");
            } else {
                ESP_LOGW(TAG, "Disconnect event received");
            }
        }
    
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}


void init_ntp(void) {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.renew_servers_after_new_IP = true;
    esp_netif_sntp_init(&config);
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    ESP_LOGI("TIME", "Waiting for SNTP sync...");
    // Block until time is set or timeout
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (err == ESP_OK) {
        ESP_LOGI("TIME", "Time synchronized");
        wifi_and_time_synced = true;
    } else {
        ESP_LOGW("TIME", "SNTP sync timeout");
        wifi_and_time_synced = false;
    }
}

esp_err_t on_client_data(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_DATA:
        // Store response in buffer (up to MAX_HTTP_BUFFER)
        int available = MAX_HTTP_BUFFER - http_buffer_len - 1;
        int copy_len = (evt->data_len < available) ? evt->data_len : available;
        if (copy_len > 0) {
            memcpy(&http_buffer[http_buffer_len], evt->data, copy_len);
            http_buffer_len += copy_len;
            http_buffer[http_buffer_len] = '\0';
        }
        ESP_LOGI("HTTP", "Received %d bytes (total: %d)", evt->data_len, http_buffer_len);
        break;

    default:
        break;
    }
    return ESP_OK;
}

static void parse_temperature(void)
{
    printf("HTTP buffer:\n%s\n", http_buffer);  // For debugging

    cJSON *root = cJSON_Parse(http_buffer);
    if (root == NULL) {
        printf("cJSON_Parse failed\n");
        ssd1306_clear_screen(oled, 0x00);
        ssd1306_draw_string(oled, 0, 20, (const uint8_t *)"JSON Error", 16, 1);
        ssd1306_refresh_gram(oled);
        return;
    }

    // Navigate: root -> "current" -> "temperature_2m"
    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!cJSON_IsObject(current)) {
        printf("No 'current' object\n");
        cJSON_Delete(root);
        return;
    }

    cJSON *temp_item = cJSON_GetObjectItem(current, "temperature_2m");
    if (!cJSON_IsNumber(temp_item)) {
        printf("No 'temperature_2m' number\n");
        cJSON_Delete(root);
        return;
    }

    float temp = (float)temp_item->valuedouble;
    latest_temp = temp;

    update_display();

    cJSON_Delete(root);
}

void fetch_weather()
{
    esp_http_client_config_t esp_http_client_config = {
        .url = "https://api.open-meteo.com/v1/forecast?latitude=41.9965&longitude=21.4314&hourly=temperature_2m&current=temperature_2m&forecast_days=1",
        .method = HTTP_METHOD_GET,
        .cert_pem = (char *) cert,
        .event_handler = on_client_data};
    esp_http_client_handle_t client = esp_http_client_init(&esp_http_client_config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI("HTTP", "HTTP GET status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        parse_temperature();
    }
    else
    {
        ESP_LOGE("HTTP", "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}


static void wifi_event_handler(void *arg, esp_event_base_t event, int32_t event_id, void *event_data){

    if (event_data == NULL) {
        ESP_LOGW(TAG, "Event %d: NULL data - skipping", event_id);
        return;
    }else if (event == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        printf("WiFi starting...\n");
    }
    if (event == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        printf("WiFi CONNECTED to AP\n");
    }
    else if (event == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "WiFi DISCONNECTED (reason: %d)", 
                 ((wifi_event_sta_disconnected_t*)event_data)->reason);
        wifi_connected = false;
        if (retry_task_handle) xTaskNotifyGive(retry_task_handle);
    } else if (event == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_connected = true;
        ESP_LOGI(TAG, "WiFi connected!");
        if(!wifi_and_time_synced) {
            init_ntp();
        }
        xTaskNotifyGive(weather_task_handle);
    }
    else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Phone joined");
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Phone left");
    }
}

static esp_err_t data_get_handler(httpd_req_t *req) {
    char query[256], value[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "value", value, sizeof(value)) == ESP_OK) {

        url_decode_inplace(value, sizeof(value));

        strncpy(http_echo_value, value, sizeof(http_echo_value) - 1);
        http_echo_value[sizeof(http_echo_value) - 1] = '\0';
        
        ESP_LOGI(TAG, "Phone echo: %s → OLED Mode 2", value);
        httpd_resp_sendstr(req, value);
        return ESP_OK;
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

// Start HTTP server bound to AP interface (192.168.4.1)
static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();  // Default HTTP server config

    // URI definition for /echo GET endpoint
    static const httpd_uri_t echo = {
        .uri       = "/echo",            // Phone calls: http://192.168.4.1/echo?value=hello
        .method    = HTTP_GET,          // GET with query parameter
        .handler   = data_get_handler,  // Handler above
        .user_ctx  = NULL
    };

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &echo);  // Register /echo handler
        ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    }
    return server;
}

void connect_ap_sta() {
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap(); 

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&config);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_start();

    esp_event_handler_instance_t any_wifi_event;
    esp_event_handler_instance_t any_ip_event;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_wifi_event);
    esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_ip_event);

    wifi_config_t wifi_config_ap = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .channel = 1,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
        
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config_ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP Started. SSID:%s password:%s IP:192.168.4.1",
            EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);

    start_webserver();

    wifi_config_t wifi_config_sta = {
        .sta = {
            .ssid = TARGET_SSID,
            .password = TARGET_PASSWORD,
        },
    };
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config_sta);

}
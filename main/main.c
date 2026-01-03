/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "wifi_handler.h"
#include <time.h>
#include "ssd1306.h"
#include "shared.h"
#include "icons.h"
#include "esp_sleep.h"  

#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 400000
#define SSD1306_I2C_ADDR 0x3C

#define NTP_SERVER "pool.ntp.org"


#define BTN_1_GPIO GPIO_NUM_4  // Button 1 (to GND, internal pullup)
#define BTN_2_GPIO GPIO_NUM_33  // Button 2 (to VCC, internal pulldown)

#define DISPLAY_MODES 3

TaskHandle_t weather_task_handle = NULL;

TaskHandle_t retry_task_handle = NULL;

static int current_mode = 0;  // 0=Time, 1=Temp, 2=HTTP text

static uint32_t last_btn_1_time = 0;
static uint32_t last_btn_2_time = 0;
static int last_btn_1_state = 1;  // Pullup = 1 (not pressed)
static int last_btn_2_state = 0;  // Pulldown = 0 (pressed)

int retry_wifi_num = 0;

bool wifi_connected = false;

char http_echo_value[128] = "The simplest things are often the best";
float latest_temp = 25.0f;
ssd1306_handle_t oled = NULL;
bool wifi_and_time_synced = false;

bool showIcons = false;

void draw_multi_line(int x, int y, const char *text, int font_size) {
    const char *pos = text;
    int line = 0;
    
    while (line < 4 && strlen(pos) > 0) {
        char line_buf[20] = {0};
        int chars_to_take = (strlen(pos) > 15) ? 15 : strlen(pos); // The width of the screen defines these numbers
        
        strncpy(line_buf, pos, chars_to_take);
        line_buf[chars_to_take] = '\0';
        
        ssd1306_draw_string(oled, x, y + (line * font_size), (const uint8_t*)line_buf, font_size, 1);
        
        pos += chars_to_take;  // Next line starts here
        line++;
    }
}

// Show no wifi icon always on top of everything
void show_no_wifi_icon(void) {
    ssd1306_draw_bitmap(oled, 105, 0, no_wifi_bitmap, 18,18);
    ssd1306_refresh_gram(oled);
}

void show_time(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
    
    if (wifi_and_time_synced) {
        ssd1306_clear_screen(oled, 0x00);
        ssd1306_draw_string(oled, 30, 25, (const uint8_t*)time_str, 16, 1);
        if (!wifi_connected) {
            show_no_wifi_icon();
        }
        ssd1306_refresh_gram(oled);
    }
}

// Show temperature (Mode 1)
void show_temperature(void) {
    char temp_str[32];
    snprintf(temp_str, sizeof(temp_str), "Temp: %.1fC", latest_temp);
    
    ssd1306_clear_screen(oled, 0x00);
    ssd1306_draw_string(oled, 0, 16, (const uint8_t*)temp_str, 16, 1);
    ssd1306_draw_string(oled, 0, 40, (const uint8_t*)"Skopje", 16, 1);
    ssd1306_refresh_gram(oled);
}

// Show HTTP text (Mode 2)
void show_http_text(void) {
    ssd1306_clear_screen(oled, 0x00);
    if (showIcons) {
        draw_multi_line(0, 0, http_echo_value, 16);
    } else {
        if (strcmp(http_echo_value, "Sun") == 0) {
            ssd1306_draw_bitmap(oled, 30, 4, sun_bitmap, 55,55);
        } else if (strcmp(http_echo_value, "Coffee") == 0) {
            ssd1306_draw_bitmap(oled, 30, 4, coffee_cup_bitmap, 55,55);
        } else if (strcmp(http_echo_value, "Snowflake") == 0) {
            ssd1306_draw_bitmap(oled, 30, 4, snowflake_bitmap, 55,55);
        } else {
            ssd1306_draw_string(oled, 0, 24, (const uint8_t*)"Missing icon...", 16, 1);
        }
    }
    ssd1306_refresh_gram(oled);
}

void oled_init(void) {
    i2c_config_t oled_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    i2c_param_config(I2C_MASTER_NUM, &oled_conf);
    i2c_driver_install(I2C_MASTER_NUM, oled_conf.mode, 0, 0, 0);

    oled = ssd1306_create(I2C_MASTER_NUM, SSD1306_I2C_ADDR);

    if (oled == NULL) {
        printf("Failed to connect library to OLED \n");
        return;
    }
    ssd1306_clear_screen(oled, 0x00);

    ssd1306_draw_string(oled, 20, 20, (const uint8_t *)"Loading...", 16, 1);

    if (!wifi_connected) {
        show_no_wifi_icon();
    }

    ssd1306_refresh_gram(oled);
}

// Update display based on mode
void update_display(void) {
    switch(current_mode) {
        case 0: {
            show_time(); break;
        }
        case 1: show_temperature(); break;
        case 2: show_http_text(); break;
    }
}

// Button 1 task - cycles modes
void button_1_task(void *pv) {
    gpio_set_direction(BTN_1_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_1_GPIO, GPIO_PULLUP_ONLY);
    
    while(1) {
        int btn_state = gpio_get_level(BTN_1_GPIO);
        
        if (btn_state == 0 && last_btn_1_state == 1) {
            uint32_t now = esp_timer_get_time() / 1000;
            if (now - last_btn_1_time > 300) {  // 300ms debounce
                current_mode = (current_mode + 1) % DISPLAY_MODES;
                ESP_LOGI("BTN 1", "Mode: %d", current_mode);
                update_display();
                last_btn_1_time = now;
            }
        }
        last_btn_1_state = btn_state;
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms poll
    }
}

void toggle_view() {
    showIcons=!showIcons;
    update_display();
}

// Button 2 task - mode functions
void button_2_task(void *pv) {
    gpio_set_direction(BTN_2_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BTN_2_GPIO, GPIO_PULLDOWN_ONLY);
    
    while(1) {
        int btn_state = gpio_get_level(BTN_2_GPIO);
        
        if (btn_state == 1 && last_btn_2_state == 0) {
            uint32_t now = esp_timer_get_time() / 1000;
            if (now - last_btn_2_time > 300) {  // 300ms debounce
                switch(current_mode) {
                    case 0: {
                        retry_wifi_num = 0;
                        if (retry_task_handle) xTaskNotifyGive(retry_task_handle);
                        break;
                    }
                    case 1: xTaskNotifyGive(weather_task_handle); break;
                    case 2: toggle_view(); break;
                }
                last_btn_2_time = now;
            }
        }
        last_btn_2_state = btn_state;
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms poll
    }
}

static void weather_task(void *params) {
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        fetch_weather();
    }
}

void app_main(void)
{
    oled_init();

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    connect_ap_sta();

    xTaskCreate(weather_task, "weather", 8192, NULL, 4, &weather_task_handle);
    xTaskCreate(wifi_retry_task, "retry", 4096, NULL, 5, &retry_task_handle);

    xTaskCreate(button_1_task, "button_oled_mode", 4096, NULL, 10, NULL);
    xTaskCreate(button_2_task, "button_mode_function", 4096, NULL, 10, NULL);


    // Main loop - always update display for time if the OLED is in mode 0
    while(1) {
        if (current_mode == 0) show_time();  // Refresh time every second
        else if (!wifi_connected) {
            show_no_wifi_icon();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
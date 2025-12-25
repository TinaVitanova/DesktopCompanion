#ifndef SHARED
#define SHARED

#include "ssd1306.h"

extern char http_echo_value[128];

extern float latest_temp;

extern ssd1306_handle_t oled;

extern void update_display(void);

extern TaskHandle_t weather_task_handle;

extern TaskHandle_t retry_task_handle;

extern bool wifi_and_time_synced;

extern int retry_wifi_num;

extern bool wifi_connected;

#endif
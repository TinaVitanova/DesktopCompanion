#ifndef WIFI_HANDLER
#define WIFI_HANDLER

#include "esp_err.h"
#include "ssd1306.h"

// Initialize wifi in AP and STA mode
void connect_ap_sta();

void fetch_weather();

void wifi_retry_task(void *params);

#endif

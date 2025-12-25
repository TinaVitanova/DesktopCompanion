idf_component_register(SRCS "main.c" "wifi_handler/wifi_handler.c" "icons/icons.c"
                       PRIV_REQUIRES spi_flash
                       REQUIRES "nvs_flash" "esp_netif" "esp_wifi" "esp_http_client" "esp_http_server" "esp_timer"
                       EMBED_TXTFILES "../CERT/open-meteo.crt"
                       INCLUDE_DIRS "." "wifi_handler" "icons")

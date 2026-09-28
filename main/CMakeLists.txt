idf_component_register(SRCS 
"main.c"
"control.c"
"adc_dac.c"
"wifinet.c"
"switch_fun.c"
"collection.c"
EMBED_FILES "meter.html.gz"
INCLUDE_DIRS ""
REQUIRES
    app_update
    driver
    esp_event
    esp_http_server
    esp_netif
    esp_system
    esp_timer
    esp_wifi
    freertos
    mdns
    nvs_flash
    esp_http_client
    mbedtls
    spi_flash
    tcpip_adapter)

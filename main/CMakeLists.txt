idf_component_register(SRCS 
"main.c"
"control.c"
"adc_dac.c"
"wifinet.c"
"switch_fun.c"
EMBED_TXTFILES "meter.html"
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
    nvs_flash
    tcpip_adapter)

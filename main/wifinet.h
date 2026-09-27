#ifndef WIFINET_H
#define WIFINET_H

#if IOT7_ENABLE_OPTICAL_PROVISIONING
extern esp_timer_handle_t config_router_timer_handle;
extern esp_timer_create_args_t config_router_periodic_arg;
extern char ls_wifi_ssid[32];
extern char ls_wifi_pass[32];
void wifi_connecting_routers(void);
#endif

extern xQueueHandle wifinet_evt_queue;

extern uint8_t progress;

extern char wifi_ssid[32];
extern char wifi_pass[64];

extern char device_ID[11];
#if IOT7_ENABLE_LEGACY_MQTT
extern char mqtt_password[33];
#endif

extern uint8_t net_state;
extern uint8_t time_state;
extern uint8_t wait;


/* Provisioning steps */
enum progress
{
    PROG_STOP = 0, //Provisioning not started
    PROG_START,    //Start receiving the start signal
    PROG_DATA,     //Start receiving data
    PROG_OVER,     //Reception complete
    PROG_ERROR,    //Reception error
    PROG_CONNECTED, //Connect to router
    PROG_CONNECTED_OK, //Router connection successful
    PROG_CONNECTED_ERR, //Router connection failed
};

enum WIFINET
{
    WIFINET_INFO = 0, //Send meter information
    WIFINET_MVOM, //Send meter reading
    WIFINET_MQTTSTOP,//Disconnect MQTT
    WIFINET_MARK,    //Mark the meter reading
    WIFINET_OTA,     //Firmware upgrade
    WIFINET_OTA_PRO, //Firmware progress
    WIFINET_CONFIG_AP, //Reopen the web provisioning hotspot
};

void wifinet_task(void *arg);
void start_config_router();
void app_wifi_initialise(void);
void wifi_config_ap_start(void);

#endif

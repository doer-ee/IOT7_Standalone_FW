#ifndef WIFINET_H
#define WIFINET_H

#include <stdbool.h>
#include <stdint.h>

extern volatile bool wifi_config_ap_active;

extern char device_ID[11];

extern uint8_t net_state;
extern uint8_t wait;


enum WIFINET
{
    WIFINET_CONFIG_AP = 0, //Open the web provisioning hotspot
    WIFINET_WAKE_STA = 1,
};

void wifinet_task(void *arg);
void request_wifi_config_ap(void);
bool wifinet_request_wake(void);

#endif

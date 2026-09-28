#ifndef MAIN_H
#define MAIN_H
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/adc.h"
#include "adc_dac.h"
#include "control.h"
#include "wifinet.h"
#include "collection.h"
#include "switch_fun.h"




#define I2C_SDA 8
#define I2C_SCL 9
#define BEEP 10
#define NET_LED 38
#define KEY 37
#define CHRG 36
#define PWR_EN 35
#define K1 17
#define K2 16
#define K3 18
#define K4 12
#define K7 15
#define K8 13

void write_config_in_nvs();
void led_task(void *arg);


#endif

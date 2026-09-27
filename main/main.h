#ifndef MAIN_H
#define MAIN_H
#include "feature_flags.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_private/system_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#if IOT7_ENABLE_SMARTCONFIG
#include "esp_smartconfig.h"
#endif
#if IOT7_ENABLE_LEGACY_MQTT
#include "mqtt_client.h"
#endif
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/rtc_io.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/twai.h" // Update from V4.2
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include <driver/spi_master.h>
#include "mbedtls/md5.h"
#include "sdmmc_cmd.h"
#include "esp_sntp.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/timer.h"
#include "driver/ledc.h"
#include "adc_dac.h"
#include "control.h"
#include "wifinet.h"
#include "switch_fun.h"
#include "ota.h"




#define I2C_SDA 8
#define I2C_SCL 9
#define BEEP 10
#define SEN 41
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

char ver[2];//Firmware version
char equipment_type[2]; // Device type
char signal_st; // Signal strength: 0-5; 5 is strongest and 0 means no signal
char net_type; // Network type: 1=Wi-Fi, 2=4G, 3=NB-IoT

void write_config_in_nvs();
void led_task(void *arg);


#endif

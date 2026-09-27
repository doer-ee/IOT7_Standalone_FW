
#include "main.h"
#include "wifinet.h"
#include "esp_http_server.h"
#include <stdbool.h>


const char *WIFINET = "WIFINET";

xQueueHandle wifinet_evt_queue;

esp_mqtt_client_handle_t client;

char server_url[]="xxx.xxxx.xxx";
char device_ID[]="0000000000";
char mqtt_password[]="312c319b9f6c104b1b9d516c0f01ef45";
char mqtt_tx_topic[]="device_txd/0000000000";
char mqtt_rx_topic[]="device_rxd/0000000000";

uint8_t net_state=0;   //Network state: 0=router disconnected, 1=router connected, 2=server connected
/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

static void smartconfig_example_task(void * parm);

uint8_t sen=0;
uint8_t add=0;
uint8_t progress=0;
uint8_t wait=0;
uint8_t time_state=0;

char wifi_ssid[32];
char wifi_pass[64];

#define CONFIG_AP_PASSWORD "iot7setup"
#define CONFIG_AP_CHANNEL  6
#define CONFIG_AP_MAX_CONN 4

static httpd_handle_t config_httpd = NULL;
static httpd_handle_t status_httpd = NULL;
static bool wifi_stack_ready = false;
static bool wifi_started = false;
static bool config_apply_started = false;
static unsigned int wifi_connect_attempts = 0;
static unsigned int wifi_disconnect_count = 0;

static esp_err_t event_handler2(void *ctx, system_event_t *event);
static esp_err_t wifi_stack_init_once(void);
static esp_err_t config_page_get_handler(httpd_req_t *req);
static esp_err_t config_save_post_handler(httpd_req_t *req);
static esp_err_t status_page_get_handler(httpd_req_t *req);
static esp_err_t status_api_get_handler(httpd_req_t *req);
static void status_httpd_start(void);
static void status_httpd_stop(void);
static void wifi_apply_task(void *arg);
static const char *wifi_disconnect_reason_name(uint8_t reason);
static void wifi_connect_with_log(const char *source);


//Pack data into Data_Buffer
void DataCombine( uint16_t com, uint8_t sn, uint8_t *data, uint16_t data_len )
{
    uint8_t Data_Buffer[2048];
    uint16_t j = 0;
    uint16_t i = 0;
    uint8_t err = 0;
    uint16_t len = data_len + 18; //Excluding payload; minimum packet length is 18
    uint8_t checksum = 0; // Checksum
    uint8_t timeS[4];//4-byte timestamp

    Data_Buffer[j] = 0xAA; // Packet header
    j++;
    Data_Buffer[j] = len >> 8; //Packet length high byte
    j++;
    Data_Buffer[j] = len & 0xff; //Packet length low byte
    j++;

    for(i = 0; i < 10; i++) //Hardware ID
    {
        Data_Buffer[j] = device_ID[i];
        j++;
    }

    Data_Buffer[j] = sn;
    j++;

    Data_Buffer[j] = com >> 8; //Command type high byte
    j++;
    Data_Buffer[j] = com & 0xff; //Command type low byte
    j++;

    for(i = 0; i < data_len; i++)
    {
        Data_Buffer[j] = data[i];
        j++;
    }

    Data_Buffer[j] = 0; // Checksum
    j++;

    Data_Buffer[j] = 0xdd; // Packet terminator
    j++;

    for(i = 0; i < j; i++) // Calculate checksum
    {
        checksum += Data_Buffer[i];
    }

    Data_Buffer[j - 2] = checksum;

    // if ((client)&&(client->state)) {
    //     ESP_LOGE(TAG, "Client was not initialized");
    //     return ESP_ERR_INVALID_ARG;
    // }
    if (client) {
        esp_mqtt_client_publish(client, mqtt_tx_topic, (char *)Data_Buffer, j, 1, 0);
    }
}

uint16_t Data_jx_com = 0;
uint8_t Data_jx_data[512];
uint16_t Data_jx_len = 0;
uint8_t Data_rxsn;

//Parse data from Buffer
uint8_t DataSeparate(uint8_t *Buffer)
{
    uint16_t len = 0;
    uint16_t j = 0;
    uint8_t he = 0; //Temporary checksum storage
    uint8_t checksum = 0; // Checksum
    uint16_t i = 0;

    if(Buffer[j] != 0xAA) //Protocol header is not 0xAA
    {
        ESP_LOGI(WIFINET,"error 1\r\n"); 
        return 0;
    }

    j++;
    len = Buffer[j]; //Packet length high byte
    j++;
    len <<= 8;
    len += Buffer[j]; //Packet length low byte
    j++;

    for (i = 0; i < 10; i++) //Device ID
    {
        if (Buffer[j] != device_ID[i]) 
        {
            ESP_LOGI(WIFINET,"error 2\r\n"); 
            return 0; //Incorrect device ID
        }

        j++;
    }

    Data_rxsn = Buffer[j];
    j++;

    Data_jx_com = Buffer[j]; //Command type high byte
    j++;
    Data_jx_com <<= 8;
    Data_jx_com += Buffer[j]; //Command type low byte
    j++;

    if((len > 500) | (len < 18)) 
    {
        ESP_LOGI(WIFINET,"error 3\r\n"); 
        return 0;
    }

    Data_jx_len = len - 18; //Payload length

    for(i = 0; i < Data_jx_len; i++) //Extract payload
    {
        Data_jx_data[i] = Buffer[j];
        j++;
    }

    he = Buffer[len - 2]; //Cache the checksum
    Buffer[len - 2] = 0; //Clear the checksum

    for(i = 0; i < len; i++)
    {
        checksum += Buffer[i];
    }

    if(he != checksum) //Incorrect checksum
    {
       // ESP_LOGI(WIFINET,"error 4\r\n"); 
        //return 0;
    }

    if(Buffer[len - 1] != 0xDD) //Protocol terminator is not 0xDD
    {
        ESP_LOGI(WIFINET,"error 5\r\n"); 
        return 0;
    }

    return 1;
}

//Convert the meter function ID to the protocol ID
uint8_t conversion_fun_id(uint8_t fun)
{
    switch (fun)
    {
        case 0x01:// DC voltage
            return 0x01;
        break;
        case 0x02:// AC voltage
            return 0x02;
        break;
        case 0x03:// DC current in mA
            return 0x04;
        break;
        case 0x04:// DC current in A
            return 0x05;
        break;
        case 0x05:// AC current in mA
            return 0x06;
        break;
        case 0x06:// AC current in A
            return 0x07;
        break;
        case 0x07://Two-wire resistance
            return 0x08;
        break;
        case 0x08://Continuity range
            return 0x0B;
        break;
        case 0x09:// DC power
            return 0x0C;
        break;
        case 0x0A:// AC power
            return 0x0D;
        break;
        case 0x0B:// Diode
            return 0x0A;
        break;
        default:
        break;
    }
    return 0x00;
}


void sntp_set_time_sync_callback(struct timeval *tv)
{
    struct tm timeinfo = {0};
    ESP_LOGI(WIFINET, "tv_sec: %lld", (uint64_t)tv->tv_sec);
    localtime_r((const time_t *)&(tv->tv_sec), &timeinfo);
    ESP_LOGI(WIFINET, "%d %d %d %d:%d:%d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    time_state=1;
    // sntp_stop();
    // utc_set_time((uint64_t)tv->tv_sec);
}

static void esp_initialize_sntp(void)
{
    char strftime_buf[64];
    ESP_LOGI(WIFINET, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_setservername(1, "ntp.ntsc.ac.cn");
    sntp_setservername(2, "edu.ntp.org.cn");
    sntp_setservername(3, "time1.cloud.tencent.com");

	sntp_set_time_sync_notification_cb(&sntp_set_time_sync_callback);
    sntp_init();


    // time_t now = 0;
    // struct tm timeinfo = { 0 };
    // int retry = 0;

    // while (timeinfo.tm_year < (2022 - 1900)) {
    //     ESP_LOGD(WIFINET, "Waiting for system time to be set... (%d)", ++retry);
    //     vTaskDelay(100 / portTICK_PERIOD_MS);
    //     time(&now);
    //     localtime_r(&now, &timeinfo);
    // }

    // // set timezone to China Standard Time
    // setenv("TZ", "CST-8", 1);
    // tzset();

    // strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    // ESP_LOGI(WIFINET, "The current date/time in Shanghai is: %s", strftime_buf);

}


void start_config_router()
{
    net_state=0;
    wait=2;
    progress=PROG_START;
    ESP_LOGI(WIFINET,"starting Wi-Fi configuration AP\r\n");
    if (wifi_stack_ready && wifinet_evt_queue != NULL) {
        uint8_t evt = WIFINET_CONFIG_AP;
        xQueueSend(wifinet_evt_queue, &evt, 0);
    }
}

uint8_t ch=0;

char ls_wifi_ssid[32];
char ls_wifi_pass[32];


void config_router_timer_cb(void *arg) 
{
    uint8_t evt;
    static uint8_t rx_data[70];
    static uint8_t rx_data_len=0;
    static uint8_t rx_len=0;
    static uint8_t rx_ssid_len=0;
    static uint8_t rx_pass_len=0;
    uint8_t checksum=0;
    uint8_t i=0;


    if(progress==PROG_START)//Wait for the start signal
    {
        if(gpio_get_level(SEN)==0)//White screen = 1
        {
            if(++add>=100)//White screen = 1 for one second
            {
                add=100;
            }
        }
        else //Black screen = 0
        {
            if(add>=100)//Start sending valid bits
            {
                esp_timer_stop(config_router_timer_handle);
                rx_data_len=0;
                progress=PROG_DATA;
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_timer_start_periodic(config_router_timer_handle, 100 * 1000);
                add=0;
                ch=0;  
                ESP_LOGI(WIFINET,"progress=PROG_DATA\r\n");
            }
            else
            {
                add=0;
            }
        }
    }
    else if(progress==PROG_DATA)
    {
        ch<<=1;
        if(gpio_get_level(SEN)==0)//White screen = 1
        {
            ch|=0x01;
        }
        add++;
        if(add>=8)
        {
            rx_data[rx_data_len++]=ch;
            if((ch==0x00)|(ch==0xFF))
            {
                ESP_LOGI(WIFINET,"error 3\r\n"); 
                esp_timer_stop(config_router_timer_handle);
                progress=PROG_ERROR;
            }

            if(rx_data_len==3)
            {
                rx_len=rx_data[0];
                rx_ssid_len=rx_data[1];
                rx_pass_len=rx_data[2];

                ESP_LOGI(WIFINET,"rx_len=%d\r\n",rx_len); 
                ESP_LOGI(WIFINET,"rx_ssid_len=%d\r\n",rx_ssid_len); 
                ESP_LOGI(WIFINET,"rx_pass_len=%d\r\n",rx_pass_len); 


                if((rx_len>68)|(rx_ssid_len>32)|(rx_pass_len>32)|(rx_len!=(rx_ssid_len+rx_pass_len+4)))//Payload length validation error
                {
                    ESP_LOGI(WIFINET,"error 1\r\n"); 
                    esp_timer_stop(config_router_timer_handle);
                    progress=PROG_ERROR; 
                    beep_start(2);
                }
            }
            ch=0;
            add=0; 
        }

        if((rx_data_len>=rx_len)&(rx_data_len>3))
        {
            checksum=0;
            for(i=0;i<(rx_len-1);i++)
            {
                checksum+=rx_data[i];
            }
            
            ESP_LOGI(WIFINET,"checksum1=%02x\r\n",checksum); 
            ESP_LOGI(WIFINET,"checksum2=%02x\r\n",rx_data[(rx_len-1)]); 
            
            if(checksum!=rx_data[(rx_len-1)])// Checksum error
            {
                ESP_LOGI(WIFINET,"error 2\r\n"); 
                esp_timer_stop(config_router_timer_handle);
                progress=PROG_ERROR;  
                beep_start(2);
            }
            else
            {
                for(i=0;i<rx_ssid_len;i++)
                {
                    ls_wifi_ssid[i]=rx_data[3+i];
                }
                ls_wifi_ssid[rx_ssid_len]=0;
                for(i=0;i<rx_pass_len;i++)
                {
                    ls_wifi_pass[i]=rx_data[3+rx_ssid_len+i];
                }
                ls_wifi_pass[rx_pass_len]=0;

                ESP_LOGI(WIFINET,"wifi_ssid=%s\r\n",ls_wifi_ssid); 
                ESP_LOGI(WIFINET,"wifi password received (%u bytes; value hidden)", (unsigned)rx_pass_len);
                esp_timer_stop(config_router_timer_handle);
                
                wifi_connecting_routers();
                progress=PROG_CONNECTED;
                beep_start(2);
                
                //progress=PROG_OVER;
            }
            
        }

    }
    
	//ESP_LOGI(WIFINET,"config_router_timer\r\n"); 
	
}  
esp_timer_handle_t config_router_timer_handle = 0;
//Define a periodically repeating timer structure
esp_timer_create_args_t config_router_periodic_arg = { 
        .callback = &config_router_timer_cb, // Callback function
		.arg = NULL, // No argument
		.name = "config_router_timer" // Timer name
		};

// In event_handler, perform the corresponding operation for each event.
static void event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
   if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
       xTaskCreate(smartconfig_example_task, "smartconfig_example_task", 4096, NULL, 3, NULL);
   } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
       esp_wifi_connect(); // Start connecting to Wi-Fi
       xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
   } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
       xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
   } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
       ESP_LOGI(WIFINET, "Scan done");
   } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
       ESP_LOGI(WIFINET, "Found channel");
   } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
       ESP_LOGI(WIFINET, "Got SSID and password");

       smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
       wifi_config_t wifi_config;
       uint8_t ssid[33] = { 0 };
       uint8_t rvd_data[33] = { 0 };
       bzero(&wifi_config, sizeof(wifi_config_t));
       memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
       memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
       wifi_config.sta.bssid_set = evt->bssid_set;

 if (wifi_config.sta.bssid_set == true) {
           memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
       }
       memcpy(ssid, evt->ssid, sizeof(evt->ssid));
       ESP_LOGI(WIFINET, "SSID:%s", ssid);
       ESP_LOGI(WIFINET, "SmartConfig password received (value hidden)");
       if (evt->type == SC_TYPE_ESPTOUCH_V2) {
           ESP_ERROR_CHECK( esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data)) );
           ESP_LOGI(WIFINET, "RVD_DATA:");
           for (int i=0; i<33; i++) {
               ESP_LOGI(WIFINET,"%02x ", rvd_data[i]);
           }
           ESP_LOGI(WIFINET,"\n");
       }
       ESP_ERROR_CHECK( esp_wifi_disconnect() );
       ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
       esp_wifi_connect();
   } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
       xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
   }
}


 void initialise_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// Create s_wifi_event_group; set event bits and handle them in the task loop.
// In smartconfig_example_task, CONNECTED_BIT means the AP is connected and ESPTOUCH_DONE_BIT means SmartConfig is complete.
static void smartconfig_example_task(void * parm)
{
   EventBits_t uxBits;
   ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS) );    // Set the SmartConfig protocol type
   smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
   ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) ); //Start one-click SmartConfig provisioning
   while (1) {
       uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
       if(uxBits & CONNECTED_BIT) {
           ESP_LOGI(WIFINET, "WiFi Connected to ap");
       }
       if(uxBits & ESPTOUCH_DONE_BIT) {
           ESP_LOGI(WIFINET, "smartconfig over");
           esp_smartconfig_stop();      // Provisioning complete; release the buffer used by esp_smartconfig_start.
           vTaskDelete(NULL);
       }
   }
}



static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    int msg_id;
    uint8_t evt=0;
    uint16_t ls_data=0;
    uint32_t identifier;

    time_t now;
    long totalSeconds;
    uint8_t tx_buf[128];
    client = event->client;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            net_state=2;
            msg_id = esp_mqtt_client_subscribe(client, mqtt_rx_topic, 0);
            ESP_LOGI(WIFINET, "sent subscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(WIFINET, "MQTT_EVENT_DISCONNECTED");
            net_state=1;
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(WIFINET, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(WIFINET, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            //ESP_LOGI(WIFINET, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA: //MQTT message received
            ESP_LOGI(WIFINET, "MQTT_EVENT_DATA");
            //ESP_LOGI(WIFINET, "Topic length: %d, data length: %d\n", event->topic_len, event->data_len);
            //ESP_LOGI(WIFINET,"TOPIC=%.*s\r\n", event->topic_len, event->topic);
            //ESP_LOGI(WIFINET,"DATA=%.*s\r\n", event->data_len, event->data);

            if(DataSeparate((uint8_t *)event->data) == 1)
            {
                ESP_LOGI(WIFINET,"Data_jx_com = 0x%04x\r\n", Data_jx_com);
                
                switch(Data_jx_com)
                {
                    case 0x0701://Get hardware information
                        evt=WIFINET_INFO;
                        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
                        break;
                    case 0x0709://Firmware upgrade
                         evt=WIFINET_OTA;
                         xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
                        break;
                    case 0x0801://Set meter parameters
                        beep_start(2);
          
                        ls_data=Data_jx_data[0];
                        ls_data<<=8;
                        ls_data+=Data_jx_data[1];
                        if((ls_data!=0xFFFF)&(ls_data>1))
                        {
                            current_freq=ls_data;
                            write_config_in_nvs(); 
                            set_current_freq();

                        }
                        if(Data_jx_data[2]!=0xFF)
                        {
                            if(Data_jx_data[2]==0x01)
                            {
                                current_fun=0x01;
                            }
                            else if(Data_jx_data[2]==0x02)
                            {   
                                current_fun=0x02;
                            }
                            else if(Data_jx_data[2]==0x04)
                            {  
                                current_fun=0x03;
                            }
                            else if(Data_jx_data[2]==0x05)
                            {
                                current_fun=0x04;
                            }
                            else if(Data_jx_data[2]==0x06)
                            {
                                current_fun=0x05;
                            }
                            else if(Data_jx_data[2]==0x07)
                            {
                                current_fun=0x06;
                            }
                            else if(Data_jx_data[2]==0x08)
                            {
                                current_fun=0x07;
                            }
                            else if(Data_jx_data[2]==0x0A)
                            {
                                current_fun=0x0B;
                            }
                            else if(Data_jx_data[2]==0x0B)
                            {
                                current_fun=0x08;
                            }
                            else if(Data_jx_data[2]==0x0C)
                            {
                                current_fun=0x09;
                            }
                            else if(Data_jx_data[2]==0x0D)
                            {
                                current_fun=0x0A;
                            }
                            write_config_in_nvs(); 
                        }  
                        DataCombine(0x0802,Data_rxsn,NULL,0);
                        break;
                    case 0x0803://Query meter parameters
                        tx_buf[0] = (current_freq>>8)&0xFF;
                        tx_buf[1] = (current_freq)&0xFF;
                        tx_buf[2] = conversion_fun_id(current_fun);
                        DataCombine(0x0804,Data_rxsn,tx_buf,3);
                        break;
                    case 0x0806://Request the current meter reading
                        evt=WIFINET_MVOM;
                        xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
                        break;
                    case 0x0807://Zero the meter
                        beep_start(2);
                        Zero_b=1;
                        write_config_in_nvs(); 
                        DataCombine(0x0808,Data_rxsn,NULL,0);
                        break;
                }
            }
           
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(WIFINET, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(WIFINET, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(WIFINET, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}


static void mqtt_app_start(void)
{
    int i=0;

    for(i=0;i<10;i++)
    {
        mqtt_tx_topic[i+11]=device_ID[i];
        mqtt_rx_topic[i+11]=device_ID[i];
    }
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .host= server_url,
            .event_handle = mqtt_event_handler_cb,//Legacy callback API; the new event callback is also registered below
            .keepalive=30,
            .port = 1883,
            .username = device_ID,
			.password = mqtt_password,
            .client_id = device_ID
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}




static EventGroupHandle_t wifi_event_group;

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        default: return "OTHER";
    }
}

static void wifi_connect_with_log(const char *source)
{
    wifi_connect_attempts++;
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGI(WIFINET, "STA connect attempt %u started by %s", wifi_connect_attempts, source);
    } else {
        ESP_LOGE(WIFINET, "STA connect attempt %u from %s failed immediately: %s (0x%x)",
                 wifi_connect_attempts, source, esp_err_to_name(err), err);
    }
}
 
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

//Wi-Fi event
static esp_err_t event_handler2(void *ctx, system_event_t *event)
{
    uint8_t evt;
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            ESP_LOGI(WIFINET, "STA started");
            wifi_connect_with_log("STA_START");
            break;
        case SYSTEM_EVENT_STA_CONNECTED:
            ESP_LOGI(WIFINET, "STA associated: channel=%u authmode=%d; waiting for DHCP",
                     (unsigned)event->event_info.connected.channel,
                     event->event_info.connected.authmode);
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(WIFINET, "STA got IP; Wi-Fi connection ready after %u attempts and %u disconnects",
                     wifi_connect_attempts, wifi_disconnect_count);
             
            net_state=1;

            // Once DHCP has completed, expose a read-only local status page
            // at the station IP.  The page remains available while the
            // device reconnects and reports the current state through JSON.
            status_httpd_start();

            if(progress==PROG_CONNECTED)
            {
                progress=PROG_CONNECTED_OK;

                // Web provisioning already committed its credentials. The old
                // optical provisioning buffers are empty in that path and must
                // not overwrite the saved NVS values after GOT_IP.
                if (ls_wifi_ssid[0] != '\0') {
                    strlcpy(wifi_ssid, ls_wifi_ssid, sizeof(wifi_ssid));
                    nvs_handle_t wificonfig_set_handle;
                    ESP_ERROR_CHECK(nvs_open("wificonfig", NVS_READWRITE, &wificonfig_set_handle));
                    ESP_ERROR_CHECK(nvs_set_str(wificonfig_set_handle, "SSID", ls_wifi_ssid));
                    ESP_ERROR_CHECK(nvs_set_str(wificonfig_set_handle, "PASSWORD", ls_wifi_pass));
                    ESP_ERROR_CHECK(nvs_commit(wificonfig_set_handle));
                    nvs_close(wificonfig_set_handle);
                    ESP_LOGI(WIFINET, "Optical Wi-Fi settings committed after connection");
                } else {
                    ESP_LOGI(WIFINET, "Web Wi-Fi settings retained after connection");
                }
            }

            if (client) {
                esp_mqtt_client_reconnect(client);
            }
            else
            {
                mqtt_app_start();
            }
            
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
            if(time_state==0) esp_initialize_sntp();
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            wifi_disconnect_count++;
            // system_event_t contains a union; casting the whole event to
            // wifi_event_sta_disconnected_t made the old log report reason 0.
            uint8_t reason = event->event_info.disconnected.reason;
            ESP_LOGW(WIFINET, "STA disconnected #%u: reason=%u (%s), SSID length=%u",
                     wifi_disconnect_count, (unsigned)reason,
                     wifi_disconnect_reason_name(reason),
                     (unsigned)event->event_info.disconnected.ssid_len);

            if(progress==PROG_CONNECTED)
            {
                progress=PROG_CONNECTED_ERR;
            }
            
            /* This is a workaround as ESP32 WiFi libs don't currently
               auto-reassociate. */
            wifi_connect_with_log("STA_DISCONNECTED");
           net_state=0;
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t wifi_stack_init_once(void)
{
    if (wifi_stack_ready) {
        return ESP_OK;
    }

    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_event_loop_init(event_handler2, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    wifi_stack_ready = true;
    return ESP_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    char copy[256];
    size_t key_len = strlen(key);
    if (out_size == 0 || strlen(body) >= sizeof(copy)) {
        return false;
    }
    strcpy(copy, body);

    char *save_ptr = NULL;
    char *field = strtok_r(copy, "&", &save_ptr);
    while (field != NULL) {
        if (strncmp(field, key, key_len) == 0 && field[key_len] == '=') {
            const char *src = field + key_len + 1;
            size_t out_len = 0;
            while (*src != '\0') {
                char decoded;
                if (*src == '+') {
                    decoded = ' ';
                    src++;
                } else if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
                    int high = hex_value(src[1]);
                    int low = hex_value(src[2]);
                    if (high < 0 || low < 0) return false;
                    decoded = (char)((high << 4) | low);
                    src += 3;
                } else {
                    decoded = *src++;
                }
                if (out_len + 1 >= out_size) {
                    return false;
                }
                if (decoded == '\0') {
                    return false;
                }
                out[out_len++] = decoded;
            }
            out[out_len] = '\0';
            return true;
        }
        field = strtok_r(NULL, "&", &save_ptr);
    }
    return false;
}

/*
 * The first version of the local UI is deliberately read-only.  Keep the
 * page small and fetch the live values from /api/status so a phone can leave
 * the page open while the device reconnects or changes signal strength.
 */
static const char status_page[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IOT7 Status</title>"
    "<style>body{font-family:system-ui,sans-serif;max-width:34rem;margin:2rem auto;padding:0 1rem;"
    "background:#f5f7fa;color:#17202a}main{background:white;border-radius:12px;padding:1.2rem;"
    "box-shadow:0 2px 12px #0001}h2{margin-top:0}dl{display:grid;grid-template-columns:8rem 1fr;"
    "gap:.55rem 1rem}dt{color:#667085}dd{margin:0;font-weight:600;word-break:break-word}"
    ".ok{color:#087f5b}.warn{color:#b54708}small{color:#667085}</style></head><body>"
    "<main><h2>IOT7 Device Status</h2><p id='state'>Loading status...</p>"
    "<dl><dt>Wi-Fi</dt><dd id='ssid'>-</dd><dt>Signal strength</dt><dd id='rssi'>-</dd>"
    "<dt>Channel</dt><dd id='channel'>-</dd><dt>IP address</dt><dd id='ip'>-</dd>"
    "<dt>Subnet mask</dt><dd id='netmask'>-</dd><dt>Gateway</dt><dd id='gateway'>-</dd>"
    "<dt>Device MAC</dt><dd id='mac'>-</dd>"
    "<dt>Device ID</dt><dd id='device'>-</dd><dt>Firmware</dt><dd id='firmware'>-</dd>"
    "<dt>Free memory</dt><dd id='heap'>-</dd><dt>Uptime</dt><dd id='uptime'>-</dd></dl>"
    "<p><small>This page refreshes every 3 seconds. Controls and measurements will be added later.</small></p></main>"
    "<script>const e=id=>document.getElementById(id);function text(id,v){e(id).textContent=v?v:'-'}"
    "function refresh(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{"
    "text('ssid',s.ssid);text('rssi',s.connected?s.rssi+' dBm':'Disconnected');"
    "text('channel',s.connected?s.channel:'-');text('ip',s.ip);text('netmask',s.netmask);"
    "text('gateway',s.gateway);"
    "text('mac',s.mac);text('device',s.device_id);text('firmware',s.firmware);"
    "text('heap',s.heap_free+' bytes');text('uptime',s.uptime_s+' s');"
    "e('state').textContent=s.connected?'Wi-Fi connected':'Wi-Fi disconnected';"
    "e('state').className=s.connected?'ok':'warn';}).catch(()=>{e('state').textContent='Failed to load status';"
    "e('state').className='warn'})}refresh();setInterval(refresh,3000);</script></body></html>";

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t used = 0;
    if (dst_size == 0) {
        return 0;
    }
    while (*src != '\0' && used + 1 < dst_size) {
        const char *replacement = NULL;
        char escaped[7];
        switch ((unsigned char)*src) {
            case '"': replacement = "\\\""; break;
            case '\\': replacement = "\\\\"; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default:
                if ((unsigned char)*src < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char)*src);
                    replacement = escaped;
                }
                break;
        }
        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (used + replacement_len >= dst_size) {
                break;
            }
            memcpy(dst + used, replacement, replacement_len);
            used += replacement_len;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    dst[used] = '\0';
    return used;
}

static void status_ip_string(const ip4_addr_t *addr, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (addr == NULL) {
        strlcpy(out, "0.0.0.0", out_size);
        return;
    }
    ip4addr_ntoa_r(addr, out, (int)out_size);
}

static esp_err_t status_api_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    bool connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && net_state == 1;

    tcpip_adapter_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    bool has_ip = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK;
    char ip[16] = "0.0.0.0";
    char netmask[16] = "0.0.0.0";
    char gateway[16] = "0.0.0.0";
    if (has_ip) {
        status_ip_string(&ip_info.ip, ip, sizeof(ip));
        status_ip_string(&ip_info.netmask, netmask, sizeof(netmask));
        status_ip_string(&ip_info.gw, gateway, sizeof(gateway));
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_string[18];
    snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_ota_get_app_description();
    char escaped_ssid[100];
    char escaped_device[32];
    char escaped_firmware[64];
    json_escape(wifi_ssid, escaped_ssid, sizeof(escaped_ssid));
    json_escape(device_ID, escaped_device, sizeof(escaped_device));
    json_escape(app != NULL ? app->version : "unknown", escaped_firmware, sizeof(escaped_firmware));

    char *json = calloc(1, 1536);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    snprintf(json, 1536,
             "{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,"
             "\"channel\":%u,\"ip\":\"%s\",\"netmask\":\"%s\","
             "\"gateway\":\"%s\",\"mac\":\"%s\",\"device_id\":\"%s\","
             "\"firmware\":\"%s\",\"heap_free\":%u,\"uptime_s\":%llu}",
             connected ? "true" : "false", escaped_ssid,
             connected ? (int)ap_info.rssi : -127,
             connected ? (unsigned)ap_info.primary : 0U,
             ip, netmask, gateway, mac_string, escaped_device, escaped_firmware,
             (unsigned)esp_get_free_heap_size(),
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t status_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, status_page, HTTPD_RESP_USE_STRLEN);
}

static void status_httpd_stop(void)
{
    if (status_httpd != NULL) {
        httpd_stop(status_httpd);
        status_httpd = NULL;
    }
}

static void status_httpd_start(void)
{
    if (status_httpd != NULL) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&status_httpd, &config) != ESP_OK) {
        status_httpd = NULL;
        ESP_LOGE(WIFINET, "failed to start Wi-Fi status web server");
        return;
    }
    static const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = status_page_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_api_get_handler,
        .user_ctx = NULL,
    };
    esp_err_t page_err = httpd_register_uri_handler(status_httpd, &page_uri);
    esp_err_t api_err = httpd_register_uri_handler(status_httpd, &api_uri);
    if (page_err != ESP_OK || api_err != ESP_OK) {
        ESP_LOGE(WIFINET, "failed to register Wi-Fi status web routes: page=%s api=%s",
                 esp_err_to_name(page_err), esp_err_to_name(api_err));
        status_httpd_stop();
        return;
    }
    ESP_LOGI(WIFINET, "Wi-Fi status Web UI ready at http://<device-ip>/");
}

static const char config_page[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IOT7 Wi-Fi</title></head><body>"
    "<h2>IOT7 Wi-Fi Settings</h2>"
    "<p>Enter your router's Wi-Fi name and password. The device will connect after saving.</p>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi name (SSID)<br><input name='ssid' maxlength='31' required></label><br><br>"
    "<label>Wi-Fi password<br><input name='password' type='password' maxlength='63'></label><br><br>"
    "<button type='submit'>Save and connect</button></form>"
    "<p>Device address: 192.168.4.1</p></body></html>";

static esp_err_t config_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, config_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form is too large");
        return ESP_FAIL;
    }

    char body[256];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char new_ssid[32] = {0};
    char new_password[64] = {0};
    if (!form_get_value(body, "ssid", new_ssid, sizeof(new_ssid)) || new_ssid[0] == '\0' ||
        !form_get_value(body, "password", new_password, sizeof(new_password))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password");
        return ESP_FAIL;
    }

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open("wificonfig", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "SSID", new_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "PASSWORD", new_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (nvs_handle) {
        nvs_close(nvs_handle);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return err;
    }

    ESP_LOGI(WIFINET, "Web Wi-Fi settings saved: SSID='%s' (%u bytes), password length=%u (value hidden)",
             new_ssid, (unsigned)strlen(new_ssid), (unsigned)strlen(new_password));

    strlcpy(wifi_ssid, new_ssid, sizeof(wifi_ssid));
    strlcpy(wifi_pass, new_password, sizeof(wifi_pass));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html lang='en'><meta charset='utf-8'><body><h2>Settings saved</h2>"
                           "<p>The device is connecting to your router's Wi-Fi. Please wait.</p></body></html>");

    if (!config_apply_started) {
        config_apply_started = true;
        if (xTaskCreate(wifi_apply_task, "wifi_apply", 4096, NULL, 5, NULL) != pdPASS) {
            config_apply_started = false;
            ESP_LOGE(WIFINET, "failed to create Wi-Fi apply task");
        }
    }
    return ESP_OK;
}

static void config_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&config_httpd, &config) != ESP_OK) {
        ESP_LOGE(WIFINET, "failed to start Wi-Fi configuration web server");
        return;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_page_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = config_save_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(config_httpd, &get_uri);
    httpd_register_uri_handler(config_httpd, &save_uri);
}

static void wifi_apply_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (config_httpd != NULL) {
        httpd_stop(config_httpd);
        config_httpd = NULL;
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));
    ESP_LOGI(WIFINET, "Switching AP to STA: SSID='%s', password length=%u (value hidden)",
             wifi_ssid, (unsigned)strlen(wifi_pass));
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(WIFINET, "esp_wifi_set_mode(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    ESP_LOGI(WIFINET, "esp_wifi_set_config(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    progress = PROG_CONNECTED;
    net_state = 0;
    wifi_connect_with_log("WEB_SAVE");
    config_apply_started = false;
    vTaskDelete(NULL);
}

static void wifi_config_ap_start_internal(void)
{
    ESP_ERROR_CHECK(wifi_stack_init_once());

    status_httpd_stop();

    if (config_httpd != NULL) {
        httpd_stop(config_httpd);
        config_httpd = NULL;
    }

    wifi_config_t ap_config;
    bzero(&ap_config, sizeof(ap_config));
    char ap_ssid[33] = {0};
    size_t id_len = strlen(device_ID);
    const char *suffix = id_len > 4 ? device_ID + id_len - 4 : device_ID;
    snprintf(ap_ssid, sizeof(ap_ssid), "IOT7-Setup-%s", suffix);
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = CONFIG_AP_CHANNEL;
    ap_config.ap.max_connection = CONFIG_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }
    progress = PROG_START;
    net_state = 0;
    ESP_LOGI(WIFINET, "Wi-Fi configuration AP: SSID=%s address=http://192.168.4.1/ (password hidden)",
             ap_ssid);
    config_httpd_start();
}

void wifi_config_ap_start(void)
{
    wifi_config_ap_start_internal();
}

// Initialize Wi-Fi STA
void app_wifi_initialise(void)
{
    nvs_handle_t wificonfig_get_handle = 0;
    esp_err_t err;
    size_t Len;
    ESP_ERROR_CHECK(wifi_stack_init_once());

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config_t));

    err = nvs_open("wificonfig", NVS_READWRITE, &wificonfig_get_handle);
    if (err == ESP_OK) {
        Len = sizeof(wifi_ssid);
        err = nvs_get_str(wificonfig_get_handle, "SSID", (char *)wifi_ssid, &Len);
        if (err == ESP_OK) {
            strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
            ESP_LOGI(WIFINET, "NVS Wi-Fi SSID='%s' (%u bytes)", wifi_ssid,
                     (unsigned)strlen(wifi_ssid));
        } else {
            ESP_LOGW(WIFINET, "NVS Wi-Fi SSID read: %s (0x%x), required buffer=%u",
                     esp_err_to_name(err), err, (unsigned)Len);
        }
        Len = sizeof(wifi_pass);
        err = nvs_get_str(wificonfig_get_handle, "PASSWORD", (char *)wifi_pass, &Len);
        if (err == ESP_OK) {
            strlcpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));
            ESP_LOGI(WIFINET, "NVS Wi-Fi password length=%u (value hidden)",
                     (unsigned)strlen(wifi_pass));
            if (strlen(wifi_pass) > 0 && strlen(wifi_pass) < 8) {
                ESP_LOGW(WIFINET, "Stored password is shorter than the WPA/WPA2 8-byte minimum; verify whether the router is open");
            }
        } else {
            ESP_LOGW(WIFINET, "NVS Wi-Fi password read: %s (0x%x), required buffer=%u",
                     esp_err_to_name(err), err, (unsigned)Len);
        }
        nvs_close(wificonfig_get_handle);
    } else {
        ESP_LOGW(WIFINET, "Wi-Fi configuration namespace is not available: %s", esp_err_to_name(err));
    }

    if (wifi_ssid[0] == '\0') {
        ESP_LOGW(WIFINET, "No Wi-Fi SSID is stored; starting configuration AP instead of STA");
        wifi_config_ap_start_internal();
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(WIFINET, "esp_wifi_set_mode(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    ESP_LOGI(WIFINET, "esp_wifi_set_config(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    if (!wifi_started) {
        err = esp_wifi_start();
        ESP_LOGI(WIFINET, "esp_wifi_start(): %s (0x%x)", esp_err_to_name(err), err);
        ESP_ERROR_CHECK(err);
        wifi_started = true;
    } else {
        wifi_connect_with_log("STA_REINITIALISE");
    }
}

// Periodically send meter hardware information
esp_timer_handle_t esp_timer_handle_txinfo = 0;
/* Timer interrupt callback */
void esp_timer_txinfo_cb(void *arg){
    uint8_t evt;

    evt=WIFINET_INFO;
    xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
}

// Connect to the router using the received router credentials
void wifi_connecting_routers(void)
{
    wifi_config_t wifi_config;

    bzero(&wifi_config, sizeof(wifi_config_t));
    //initialise_wifi();
    memcpy(wifi_config.sta.ssid, ls_wifi_ssid, sizeof(ls_wifi_ssid));
    memcpy(wifi_config.sta.password, ls_wifi_pass, sizeof(ls_wifi_pass));

    // Connect to the configured wireless network
    ESP_ERROR_CHECK( esp_wifi_disconnect() );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_connect() );
}



void wifinet_task(void *arg)
{
    uint8_t evt;
    time_t now;
    int i=0;
    long totalSeconds;
    uint8_t tx_buf[128];
    uint8_t sn_count=0;
    uint8_t fun=0;
    wifinet_evt_queue = xQueueCreate(3, sizeof(uint8_t));
    //vTaskDelay(pdMS_TO_TICKS(1000));
    //initialise_wifi();                   // Initialize Wi-Fi in STA mode and wait for app provisioning
    esp_timer_create(&config_router_periodic_arg, &config_router_timer_handle);
    while(wait==0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (wait == 2) {
        wifi_config_ap_start();
    } else {
        app_wifi_initialise();
    }
    

        // Initialize the timer structure
    esp_timer_create_args_t esp_timer_create_args_txinfo = {
        .callback = &esp_timer_txinfo_cb, // Timer callback function
        .arg = NULL, // Callback argument
        .name = "esp_timer_txinfo" // Timer name
    };

    /* Create timer */
    esp_err_t err = esp_timer_create(&esp_timer_create_args_txinfo, &esp_timer_handle_txinfo);
    err = esp_timer_start_periodic(esp_timer_handle_txinfo, 3000 * 1000);
    while (1) 
    {
        if (xQueueReceive(wifinet_evt_queue, &evt, portMAX_DELAY))
        {
            switch (evt)
            {
                case WIFINET_CONFIG_AP:
                    wifi_config_ap_start();
                    break;

                case WIFINET_INFO: //Send meter information

                    switch (current_fun)
                    {
                        fun=0;
                    }
                    time(&now);
                    totalSeconds=(long)now;
                    //ESP_LOGI(WIFINET,"totalSeconds = %ld\r\n", totalSeconds);
                    tx_buf[0] = (totalSeconds >> 24) & 0XFF;
                    tx_buf[1] = (totalSeconds >> 16) & 0XFF;
                    tx_buf[2] = (totalSeconds >> 8) & 0XFF;
                    tx_buf[3] = (totalSeconds) & 0XFF;
                    tx_buf[4] =  ver[0];// Version
                    tx_buf[5] =  ver[1];// Version
                    tx_buf[6] =  equipment_type[0];// Device type
                    tx_buf[7] =  equipment_type[1];// Device type
                    tx_buf[8] =  electricity_st;// Battery level
                    tx_buf[9] =  signal_st;// Signal strength
                    tx_buf[10] =  net_type;// Network type
                    tx_buf[11] =  fun;// Current function
                    DataCombine(0x0702,Data_rxsn,tx_buf,12);
                break;

                case WIFINET_MVOM: //Send meter reading
                    //ESP_LOGI(WIFINET, "\r\n--------WIFINET_MVOM ---------");
                    time(&now);
                    totalSeconds=(long)now;
                    tx_buf[0] = (current_freq>>8)&0xFF;
                    tx_buf[1] = (current_freq)&0xFF;
                    tx_buf[2]=conversion_fun_id(current_fun);
                    tx_buf[3]=sign;
                    tx_buf[4] = (measured_value >> 24) & 0XFF;
                    tx_buf[5] = (measured_value >> 16) & 0XFF;
                    tx_buf[6] = (measured_value >> 8) & 0XFF;
                    tx_buf[7] = (measured_value) & 0XFF;
                    tx_buf[8] =  unit;
                    tx_buf[9] = (totalSeconds >> 24) & 0XFF;
                    tx_buf[10] = (totalSeconds >> 16) & 0XFF;
                    tx_buf[11] = (totalSeconds >> 8) & 0XFF;
                    tx_buf[12] = (totalSeconds) & 0XFF;
                    DataCombine(0x0805,sn_count++,tx_buf,13);   
                break;

                case WIFINET_MQTTSTOP: //Disconnect MQTT
                esp_mqtt_client_stop(client);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_mqtt_client_stop(client);
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_mqtt_client_stop(client);
                break;

                case WIFINET_MARK: //Mark the meter reading
                    tx_buf[0]=0x01;
                    DataCombine(0x080B,sn_count++,tx_buf,1);   
                break;

                case WIFINET_OTA: // Firmware upgrade
                     xTaskCreate(&ota_task, "ota_task", 1024 * 8, NULL, 20, NULL);
                break;

                case WIFINET_OTA_PRO: // Firmware upgrade progress
                    tx_buf[0]=percentage;
                    DataCombine(0x070A,sn_count++,tx_buf,1);   
                break;
            }
            
        }
    }

}

/* Screen Example

   For other examples please check:
   https://github.com/espressif/esp-iot-solution/tree/master/examples

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
 */


#include "main.h"


static const char *TAG = "MAIN";



static const char ver[] = "A2"; //Firmware version

uint8_t net_led_add=0;

uart_config_t uart_config = {
       .baud_rate = 115200,
       .data_bits = UART_DATA_8_BITS,
       .parity = UART_PARITY_DISABLE,
       .stop_bits = UART_STOP_BITS_1,
       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
       .source_clk = UART_SCLK_APB,
};
static const int RX_BUF_SIZE = 1024;

void usart0_init() {  
    //We won't use a buffer for sending data.
   uart_driver_install(UART_NUM_0, RX_BUF_SIZE, 0, 0, NULL, 0);
   uart_param_config(UART_NUM_0, &uart_config);
   uart_set_pin(UART_NUM_0, 43, 44, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}






/*Define the union type*/
typedef union {
	uint32_t uint32val;
	int intval;
	float floatval;
}NUION32;



void gpio_init(void)
{

   gpio_pad_select_gpio(BEEP);
   gpio_set_direction(BEEP, GPIO_MODE_OUTPUT);
   gpio_set_level(BEEP, 0);

   gpio_pad_select_gpio(NET_LED);
   gpio_set_direction(NET_LED, GPIO_MODE_OUTPUT);
   gpio_set_level(NET_LED, 0);

   gpio_pad_select_gpio(PWR_EN);
   gpio_set_direction(PWR_EN, GPIO_MODE_OUTPUT);
   /* Keep the hardware power latch enabled as soon as GPIO is configured. */
   gpio_set_level(PWR_EN, 1);

   gpio_pad_select_gpio(K1);
   gpio_set_direction(K1, GPIO_MODE_OUTPUT);
   gpio_set_level(K1, 0);

   gpio_pad_select_gpio(K2);
   gpio_set_direction(K2, GPIO_MODE_OUTPUT);
   gpio_set_level(K2, 0);

   gpio_pad_select_gpio(K3);
   gpio_set_direction(K3, GPIO_MODE_OUTPUT);
   gpio_set_level(K3, 0);

   gpio_pad_select_gpio(K4);
   gpio_set_direction(K4, GPIO_MODE_OUTPUT);
   gpio_set_level(K4, 0);

   gpio_pad_select_gpio(K7);
   gpio_set_direction(K7, GPIO_MODE_OUTPUT);
   gpio_set_level(K7, 0);

   gpio_pad_select_gpio(K8);
   gpio_set_direction(K8, GPIO_MODE_OUTPUT);
   gpio_set_level(K8, 0);

   gpio_pad_select_gpio(KEY);
   gpio_set_direction(KEY, GPIO_MODE_INPUT);
   gpio_pullup_en(KEY);

   gpio_pad_select_gpio(CHRG);
   gpio_set_direction(CHRG, GPIO_MODE_INPUT);
   gpio_pullup_en(CHRG);

}



void usart0_task(void *arg)
{
   char* ls;
   char* data = (char*) malloc(RX_BUF_SIZE+1);
   int i=0;


   while (1) {
      const int rxBytes = uart_read_bytes(UART_NUM_0, data, RX_BUF_SIZE, 20 / portTICK_RATE_MS);
      if (rxBytes > 0) 
      {
         data[rxBytes]=0;

         ls=strstr(data,"fun=dcv");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            control_submit_function(1, NULL);
            beep_start(2);
         }

         ls=strstr(data,"fun=dcma");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            control_submit_function(3, NULL);
            beep_start(2);
         }

         ls=strstr(data,"fun=dca");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            control_submit_function(4, NULL);
            beep_start(2);
         }

         ls=strstr(data,"fun=r");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            control_submit_function(7, NULL);
            beep_start(2);
         }

         ls=strstr(data,"zero");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            control_submit_zero(NULL);
            beep_start(2);
         }

         ls=strstr(data,"slope");
         if(ls!=0)
         {
            printf( "%s OK\r\n", data);
            Slope_b=1;
            beep_start(2);
         }

         ls=strstr(data,"ID=");
         if(ls!=0)
         {
            for(i=0;i<10;i++)
            {
               device_ID[i]=ls[3+i];
            }
            write_config_in_nvs(); 
            printf( "device_ID=%s\r\n", device_ID);
            beep_start(2);
         }

      }
   }
}




uint8_t Already_saved=0x34;
void write_config_in_nvs()
{
    nvs_handle_t config_get_handle;
    NUION32 union32;

    nvs_open("parameter", NVS_READWRITE, &config_get_handle);
    ESP_ERROR_CHECK( nvs_set_u8(config_get_handle,"Alreadysaved", Already_saved) );

    ESP_ERROR_CHECK( nvs_set_str(config_get_handle,"device_ID",(const char *)device_ID) );
    ESP_ERROR_CHECK( nvs_set_u8(config_get_handle,"c_fun", current_fun) );

    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_0", Zero[0]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_1", Zero[1]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_2", Zero[2]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_3", Zero[3]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_4", Zero[4]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_5", Zero[5]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_6", Zero[6]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_7", Zero[7]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_8", Zero[8]) );
    ESP_ERROR_CHECK( nvs_set_i32(config_get_handle,"Zero_9", Zero[9]) );

    union32.floatval=Slope[0];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_0", union32.uint32val));
    union32.floatval=Slope[1];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_1", union32.uint32val));
    union32.floatval=Slope[2];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_2", union32.uint32val));
    union32.floatval=Slope[3];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_3", union32.uint32val));
    union32.floatval=Slope[4];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_4", union32.uint32val));
    union32.floatval=Slope[5];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_5", union32.uint32val));
    union32.floatval=Slope[6];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_6", union32.uint32val));
    union32.floatval=Slope[7];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_7", union32.uint32val));
    union32.floatval=Slope[8];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_8", union32.uint32val));
    union32.floatval=Slope[9];
    ESP_ERROR_CHECK( nvs_set_u32(config_get_handle,"Slope_9", union32.uint32val));


    nvs_close(config_get_handle);
}

static void read_config_in_nvs(void)
{
    nvs_handle_t config_get_handle;
    uint8_t u8ConfigVal;
    size_t Len;
    NUION32 union32;

    nvs_open("parameter", NVS_READWRITE, &config_get_handle);
    nvs_get_u8(config_get_handle, "Alreadysaved", &u8ConfigVal);
     ESP_LOGE(TAG,"u8ConfigVal:%X \r\n",u8ConfigVal);
    if (u8ConfigVal == Already_saved)
    {
        ESP_LOGI(TAG, "----Get parameter OK----");

        Len = sizeof(device_ID);
        nvs_get_str(config_get_handle, "device_ID", (char *)device_ID, &Len);


        nvs_get_u8(config_get_handle, "c_fun", &current_fun);


        nvs_get_i32(config_get_handle, "Zero_0", &Zero[0]);
        nvs_get_i32(config_get_handle, "Zero_1", &Zero[1]);
        nvs_get_i32(config_get_handle, "Zero_2", &Zero[2]);
        nvs_get_i32(config_get_handle, "Zero_3", &Zero[3]);
        nvs_get_i32(config_get_handle, "Zero_4", &Zero[4]);
        nvs_get_i32(config_get_handle, "Zero_5", &Zero[5]);
        nvs_get_i32(config_get_handle, "Zero_6", &Zero[6]);
        nvs_get_i32(config_get_handle, "Zero_7", &Zero[7]);
        nvs_get_i32(config_get_handle, "Zero_8", &Zero[8]);
        nvs_get_i32(config_get_handle, "Zero_9", &Zero[9]);


        nvs_get_u32(config_get_handle, "Slope_0", &union32.uint32val);
        Slope[0]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_1", &union32.uint32val);
        Slope[1]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_2", &union32.uint32val);
        Slope[2]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_3", &union32.uint32val);
        Slope[3]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_4", &union32.uint32val);
        Slope[4]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_5", &union32.uint32val);
        Slope[5]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_6", &union32.uint32val);
        Slope[6]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_7", &union32.uint32val);
        Slope[7]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_8", &union32.uint32val);
        Slope[8]=union32.floatval;
        nvs_get_u32(config_get_handle, "Slope_9", &union32.uint32val);
        Slope[9]=union32.floatval;

        nvs_close(config_get_handle);

    }
    else
    {
        ESP_LOGI(TAG, "----Get parameter Fail----");
        nvs_close(config_get_handle);
        write_config_in_nvs();  
    }

}


void led_task(void *arg)
{

	while (1) 
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        if(wifi_config_ap_active) //Web provisioning AP is active
        {
            gpio_set_level(NET_LED, 0);
        }
        else if(net_state == 0) // Router disconnected: LED on for 0.8 seconds, off for 0.2 seconds
        {
            if(net_led_add == 0)
            {
                gpio_set_level(NET_LED, 0);
            }
            else if(net_led_add == 8)
            {
                gpio_set_level(NET_LED, 1);
            }

            if(++net_led_add > 9) net_led_add = 0;
        }
        else if(net_state == 1) // Router connected: LED on for 0.2 seconds, off for 0.8 seconds
        {
            if(net_led_add == 0)
            {
                gpio_set_level(NET_LED, 0);
            }
            else if(net_led_add == 2)
            {
                gpio_set_level(NET_LED, 1);
            }

            if(++net_led_add > 9) net_led_add = 0;
        }

        // while(gpio_get_level(PWR_KEY)==0)
        // {
        //     ESP_LOGI(TAG, "key");
        //     vTaskDelay(pdMS_TO_TICKS(500));
        //     gpio_set_level(PWR_EN, 0);
        // }


 //if(gpio_get_level(CHRG)==0) ESP_LOGI(TAG, "\r\n  CHRG  \r\n");


   }
}



void app_main(void)
{
    gpio_init();
    //pwm_ledc_init();
    usart0_init();
      esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
    ESP_ERROR_CHECK(ret);
    read_config_in_nvs();//Read configuration
    collection_init();
    control_init();
    printf("Version=%s\n", ver);
    printf("device_ID = %s\n",device_ID);

    xTaskCreate(adc_task, "ADC", 1024*2, NULL, 3, NULL);
    xTaskCreate(wifinet_task, "WIFINET", 1024*6, NULL, 7, NULL);
    xTaskCreate(switch_fun_task, "SWITCH", 1024*2, NULL, 14, NULL); 
    xTaskCreate(usart0_task, "usart0", 1024*2, NULL, 16, NULL);
    vTaskDelay(pdMS_TO_TICKS(5000));
    printf(" ------------------esp_get_free_heap_size : %d \n", esp_get_free_heap_size());

   // char* p = (char*)malloc(10240);
   // free(p);

   //Get remaining DRAM
	size_t dram = heap_caps_get_free_size(MALLOC_CAP_8BIT);

	//Get remaining IRAM
	size_t iram = heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT);

	//Get remaining DRAM, equivalent to heap_caps_get_free_size
	uint32_t data = xPortGetFreeHeapSize();


	//Get the largest contiguous heap block
	size_t heapmax = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

	//Get the stack high-water mark (top of stack)
	int stackmark=uxTaskGetStackHighWaterMark(NULL);
	
	printf("data=%d\n", data);
	printf("dram=%d\n", dram);
	printf("iram=%d\n", iram);
	printf("max=%d\n", heapmax);
	printf("stackmark=%d\n", stackmark);

   

    
}

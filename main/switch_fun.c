#include "main.h"
#include "switch_fun.h"

const char *SWITCH_FUN = "SWITCH_FUN";

uint8_t current_fun = 1; // Current function: 01 DC voltage, 02 AC voltage, 03 DC mA, 04 DC A, 05 AC mA, 06 AC A, 07 resistance, 08 continuity, 09 DC power, 0A AC power, 0B diode
uint8_t current_fun_old=0xFF;
uint8_t current_sw=0;// Current range state
static volatile bool range_locked;
static volatile uint8_t locked_range;
//Switch the analog multiplexer
void analog_switch(uint8_t sw)
{
	if(range_locked && sw != locked_range)
	{
		return;
	}
	if(current_sw!=sw)
	{
		current_sw=sw;
		switch(sw)
		{
			case ASW_OFF:	
			break;
         case ASW_DCV1://Maximum 1000 V
            gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);
			break;
			case ASW_DCV2://Maximum 100 V
            gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			case ASW_DCV3://Maximum 10 V
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			case ASW_ACV1://Maximum 1000 V
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 1); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);
			break;
			case ASW_ACV2://Maximum 100 V
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 1); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			case ASW_ACV3://Maximum 10 V
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 1); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			
			case ASW_DCMA://Maximum 250 mA
				gpio_set_level(K1, 1); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			
			case ASW_DCA://Maximum 2.5 A
				gpio_set_level(K1, 1); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 0);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			
			case ASW_ACMA://Maximum 250 mA
				gpio_set_level(K1, 1); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 1); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			
			case ASW_ACA://Maximum 2.5 A
				gpio_set_level(K1, 1); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 0);
            gpio_set_level(K3, 1); // 1 = AC
				gpio_set_level(K4, 0);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			case ASW_R2://1M
            gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 1);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			case ASW_R3://100K
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 1);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 1);// K8/K7 divider
			break;
			case ASW_R4://10K
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 1);// 1 = resistance
				gpio_set_level(K7, 1);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			case ASW_R5://1K
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 1);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;
			case ASW_BEEP:// Continuity buzzer
				gpio_set_level(K1, 0); //K2K1 00:GND  01:A  10:V  11:mA
            gpio_set_level(K2, 1);
            gpio_set_level(K3, 0); // 1 = AC
				gpio_set_level(K4, 1);// 1 = resistance
				gpio_set_level(K7, 0);// K8/K7 divider: 00=1K, 01=10K, 10=100K, 11=1M
				gpio_set_level(K8, 0);// K8/K7 divider
			break;

		}
	}
}

void switch_range_lock(bool locked, uint8_t range)
{
	range_locked = locked;
	locked_range = locked ? range : ASW_OFF;
}

bool switch_range_is_locked(uint8_t *range)
{
	bool locked = range_locked;
	if(range != NULL)
	{
		*range = locked_range;
	}
	return locked;
}


uint8_t K1Set,K1Cnt;
#define POWER_ON_HOLD_TICKS 5
void KeyScan()
{

		if(gpio_get_level(KEY)==0) 
		{		
         
         if(++K1Cnt==3) 
         {
               K1Set = 1;			
         }
         else if(K1Cnt==40)
         { 
               ESP_LOGI(SWITCH_FUN, "Press and hold the key");
               K1Set=0; 
               gpio_set_level(BEEP, 1);
               vTaskDelay(pdMS_TO_TICKS(100));
               gpio_set_level(BEEP, 0);
               vTaskDelay(pdMS_TO_TICKS(80));
               gpio_set_level(BEEP, 1);
               vTaskDelay(pdMS_TO_TICKS(100));
               gpio_set_level(BEEP, 0);
               gpio_set_level(PWR_EN, 0);
         }
         else if(K1Cnt>40)
         {
               K1Cnt=100;
         }
         
		}
		else
		{
			 K1Cnt = 0;
			 if(K1Set==1)
			 {
					K1Set = 0;
               gpio_set_level(BEEP, 1);
               vTaskDelay(pdMS_TO_TICKS(100));
               gpio_set_level(BEEP, 0);

               if(net_state == 1) //Mark the measurement while connected to the local network
               {
                  control_submit_mark(NULL);
               } 
			 }
		}   
}

uint8_t beep_add=0;
uint8_t beep_k=0;
uint8_t beep_d=0;

void beep_start(uint8_t duration)
{
   beep_d=duration;
   beep_add=0;
   beep_k=1;
}

void Beep_drive()
{
   if(beep_k==1)
   {
      if(++beep_add>beep_d)
      {
         gpio_set_level(BEEP, 0);
         beep_k=0;
      }
      else if(beep_add==1)
      {
         gpio_set_level(BEEP, 1);
      }
   }
}

esp_timer_handle_t esp_timer_handle_t1 = 0;
/* Timer interrupt callback */
void esp_timer_cb(void *arg){
   KeyScan();
   Beep_drive();
}


void switch_fun_task(void *arg)
{
   uint8_t key_add = 0;

   wait=0;

   while(1)//Hold briefly to power on
   {
      vTaskDelay(pdMS_TO_TICKS(90));
      if(++key_add >= POWER_ON_HOLD_TICKS) break;
      if((gpio_get_level(KEY)!=0)) key_add = 0;
   }
   gpio_set_level(PWR_EN, 1);
   gpio_set_level(BEEP, 1);
   vTaskDelay(pdMS_TO_TICKS(200));
   gpio_set_level(BEEP, 0);
   
   
   key_add = 0;
  
   while(1)//Long press to power on
   {
      if((gpio_get_level(KEY)!=0)) 
      {
         wait=1;
         break;
      }
      vTaskDelay(pdMS_TO_TICKS(100));

      if(key_add < 20)
      {
         key_add++;
      }
      else //Hold the button for about 3 seconds after power-on to enter the web provisioning hotspot
      {
         ESP_LOGI(SWITCH_FUN, "LONG KEY");
         wait=2;
         request_wifi_config_ap();
         gpio_set_level(BEEP, 1);
         vTaskDelay(pdMS_TO_TICKS(1000));
         gpio_set_level(BEEP, 0);
         break;
      }
   }
		
	 key_add = 0;
    while((gpio_get_level(KEY)==0))
    {
        vTaskDelay(pdMS_TO_TICKS(100));

        if(key_add < 70)
        {
            key_add++;
        }
        else //Hold for more than 10 seconds to restore factory settings
        {
            ESP_LOGI(SWITCH_FUN, "LONG LONG KEY"); 
            while((gpio_get_level(KEY)==0))
            {
               vTaskDelay(pdMS_TO_TICKS(100));
            }			
        }
    }

    xTaskCreate(led_task, "led", 1024*2, NULL, 15, NULL);

     // Initialize the timer structure
    esp_timer_create_args_t esp_timer_create_args_t1 = {
        .callback = &esp_timer_cb, // Timer callback function
        .arg = NULL, // Callback argument
        .name = "esp_timer" // Timer name
    };

    /* Create timer */
    esp_timer_create(&esp_timer_create_args_t1, &esp_timer_handle_t1);
    esp_timer_start_periodic(esp_timer_handle_t1, 30 * 1000);

   //ESP_LOGI(SWITCH_FUN, "fun_mux(SW_FUN_CAN)");
    
    while (1) {

      vTaskDelay(pdMS_TO_TICKS(1000));
      //ESP_LOGI(SWITCH_FUN, "switch_fun_task");
      
   }

}


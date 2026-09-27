
#include "main.h"
#include "adc_dac.h"

#define    PRINTF_VALUE    0  //Enable measurement logging


const char *ADC_TASK_TAG = "ADC_TASK";

uint16_t current_freq = 5; //Measurement period in 0.1-second units
uint8_t electricity_st = 100; // Battery level: 0=low, 255=normal, 254=charging, 1-100=percentage
uint32_t measured_value = 0; //Measurement value; all Fs indicate overrange
uint8_t sign = 0; //Sign; 1 indicates negative
uint8_t unit = 0; //Measurement unit

int Zero[10]={0,0,0,0,0,0,0,0,0,0}; //Zero calibration values
uint8_t  Zero_b=0;  //Start zero calibration
float Slope[10]={1,1,1,1,1,1,1,1,1,1};//Slope calibration
uint8_t  Slope_b=0;

/****************************************************************************
Function name: MCP3421_WriteReg
Input parameters: none
Return value: none
Description:
****************************************************************************/
void MCP3421_WriteReg(uint8_t writeData)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
   i2c_master_start(cmd);
   i2c_master_write_byte(cmd, 0xD0, 1);
   i2c_master_write_byte(cmd, writeData, 1);
   i2c_master_stop(cmd);
   esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_RATE_MS);
   if (ret != ESP_OK) {
       ESP_LOGI(ADC_TASK_TAG, "MCP3421_WriteReg ERROR =%d",ret);
   }
   i2c_cmd_link_delete(cmd);
}

/****************************************************************************
Function name: MCP3421_ReadReg
Input parameters: none
Return value: none
Description:
****************************************************************************/
uint32_t MCP3421_ReadReg(void)
{
    uint8_t elec[4], mcp_busy;
 
   i2c_cmd_handle_t cmd = i2c_cmd_link_create();
   i2c_master_start(cmd);
   i2c_master_write_byte(cmd, 0xD1, 1);
   i2c_master_read(cmd, &elec[0],1,0x00);
   i2c_master_read(cmd, &elec[1],1,0x00);
   i2c_master_read(cmd, &elec[2],1,0x00);
   i2c_master_read(cmd, &elec[3],1,0x01);
   i2c_master_stop(cmd);
   esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000 / portTICK_RATE_MS);
   if (ret != ESP_OK) {
       ESP_LOGI(ADC_TASK_TAG, "MCP3421_ReadReg ERROR =%d",ret);
   }
   i2c_cmd_link_delete(cmd);

   //ESP_LOG_BUFFER_HEX(ADC_TASK_TAG, dat, 4);
   return ((uint32_t)((uint32_t)elec[0] << 16 | (uint32_t)elec[1] << 8 | elec[2])) & 0x03FFFF;

}

void I2C_Init()
{	
   esp_err_t ret;
   i2c_config_t conf = {};
   conf.mode = I2C_MODE_MASTER;
   conf.sda_io_num = I2C_SDA;
   conf.scl_io_num = I2C_SCL;
   conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
   conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
   conf.master.clk_speed = 200000;
   i2c_param_config(I2C_NUM_0, &conf);
   i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
}



//Measure DC voltage
void measure_dcv(uint32_t ad_dat)
{
	  int64_t Vol;
	  int lsdat=0;
	  int dat=0;
	
		dat=((int)(ad_dat<<14));
	/***Zero the DC voltage range***/
	  if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[0]=dat;
			  write_config_in_nvs();
		}
	
	  dat-=Zero[0];
	/***************/	
		
		lsdat = abs(dat);  //Take the absolute value of a
		
		Vol=lsdat*0.00095367; //Compute voltage in microvolts
		
		
		if(current_sw==ASW_DCV1)//Maximum 1000 V
		{
			  Vol*=525;
			
			  if(Slope_b!=0)
				{ 
				   Slope[0]=99359000.0/Vol; 
					 ESP_LOGI(ADC_TASK_TAG,"Slope[0]=%f\r\n",Slope[0]);
					 Slope_b=0;
					write_config_in_nvs();
				}
				Vol*=Slope[0];
				
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"DCV1 Vol=%lld\r\n",Vol);
			  if(Vol<70000000)//Switch range below 70 V
				{
						analog_switch(ASW_DCV2);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();

				}
		}
		else if(current_sw==ASW_DCV2)//Maximum 100 V
		{
			  Vol*=52.55;
			
			  if(Slope_b!=0)
				{ 
				   Slope[1]=49752000.0/Vol; 
					 ESP_LOGI(ADC_TASK_TAG,"Slope[1]=%f\r\n",Slope[1]);
					 Slope_b=0;
					 write_config_in_nvs();
				}
				Vol*=Slope[1];
				
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"DCV2 Vol=%lld\r\n",Vol);
			  if(Vol<8000000)//Switch range below 8 V
				{
						analog_switch(ASW_DCV3);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();
				}
				else if(Vol>65000000)//Switch range below 65 V
				{
						analog_switch(ASW_DCV1);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();

				}
		}
		else if(current_sw==ASW_DCV3)//Maximum 10 V
		{
			  Vol*=6.148;
			
			  if(Slope_b!=0)
				{ 
				   Slope[2]=7497000.0/Vol; 
					 ESP_LOGI(ADC_TASK_TAG,"Slope[2]=%f\r\n",Slope[2]);
					 Slope_b=0;
					 write_config_in_nvs();
				}
				Vol*=Slope[2];
			   
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"DCV3 Vol=%lld\r\n",Vol);
			  if(Vol>9000000)//Switch range above 9 V
				{
						analog_switch(ASW_DCV2);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();
				}
		}
		else
		{
		    analog_switch(ASW_DCV2);
		}
		
		if(dat>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}

		if(Vol<10000000)// Voltage below 10 V
    {
          unit=0x01;
          measured_value=Vol;
    }
    else //mV
    {
          unit=0x02;
          measured_value=Vol/1000;
    }
	
}

//Measure AC voltage
void measure_acv(uint32_t ad_dat)
{
  int64_t Vol;
	  int lsdat=0;
	  int dat=0;
	
		dat=((int)(ad_dat<<14));
	/***Zero the AC voltage range***/
	 if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[1]=lsdat;
			  write_config_in_nvs();
		}
	
	  lsdat-=Zero[1];
	/***************/			
		lsdat = abs(dat);  //Take the absolute value of a
		Vol=lsdat*0.00095367; //Compute voltage in microvolts
		if(current_sw==ASW_ACV1)//Maximum 1000 V
		{
			  Vol*=525;
				Vol*=Slope[0];
        //Vol*=1.116;
				
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ACV1 Vol=%lld\r\n",Vol);
			  if(Vol<24000000)//Switch range below 24 V
				{
						analog_switch(ASW_ACV2);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();

				}
		}
		else if(current_sw==ASW_ACV2)//Maximum 100 V
		{
			  Vol*=52.55;
				Vol*=Slope[1];
        //Vol*=1.116;
				
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ACV2 Vol=%lld\r\n",Vol);
			  if(Vol<3000000)//Switch range below 8 V
				{
						analog_switch(ASW_ACV3);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();
				}
				else if(Vol>25000000)//Switch range below 25 V
				{
						analog_switch(ASW_ACV1);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();

				}
		}
		else if(current_sw==ASW_ACV3)//Maximum 10 V
		{
			  Vol*=6.148;
				Vol*=Slope[2];
        //Vol*=1.116;
			  
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ACV3 Vol=%lld\r\n",Vol);
			  if(Vol>4000000)//Switch range above 4 V
				{
						analog_switch(ASW_ACV2);
						vTaskDelay(pdMS_TO_TICKS(100));
						//MCP3421_WriteReg(0x8C);
						vTaskDelay(pdMS_TO_TICKS(100));
						MCP3421_ReadReg();
				}
		}
		else
		{
		    analog_switch(ASW_ACV2);
		}
		
		if(dat>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}
		
		//Vol=labs(Vol);
		if(Vol<10000000)// Voltage below 10 V
    {
          unit=0x01;
          measured_value=Vol;
    }
    else //mV
    {
          unit=0x02;
          measured_value=Vol/1000;
    }

}

//Measure DC current in mA
void measure_dcma(uint32_t ad_dat)
{
	  int64_t Aol;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	/***Zero the DC current range***/
	 if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[2]=lsdat;
			  write_config_in_nvs();
		}
	
	  lsdat-=Zero[2];
	/***************/	
		
		Aol=lsdat*0.00095367; //Compute voltage in microvolts
		

		if(current_sw==ASW_DCMA)//Maximum 250 mA
		{
			  Aol*=0.35;
			  if(Slope_b!=0)
				{ 
					 Slope[3]=250000.0/Aol; 
					 ESP_LOGI(ADC_TASK_TAG,"Slope[3]=%f\r\n",Slope[3]);
					 Slope_b=0;
					 write_config_in_nvs();
				}
				Aol*=Slope[3];
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ASW_DCMA Aol=%lld\r\n",Aol);
			  
		}	
		else
		{
		    analog_switch(ASW_DCMA);
		}
		
		if(Aol>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}
		
		Aol=labs(Aol);
		unit=0x05;
		measured_value=Aol;
		
	
}

//Measure DC current in A
void measure_dca(uint32_t ad_dat)
{
	  int64_t Aol;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	/***Zero the DC current range***/
	  if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[3]=lsdat;
			  write_config_in_nvs();
		}
	
	  lsdat-=Zero[3];
	/***************/	
		
		Aol=lsdat*0.00095367; //Compute voltage in microvolts

		if(current_sw==ASW_DCA)//Maximum 2.5 A
		{
			  Aol*=6.7;
			  if(Slope_b!=0)
				{ 
					 Slope[4]=1002000.0/Aol; 
					 ESP_LOGI(ADC_TASK_TAG,"Slope[4]=%f\r\n",Slope[4]);
					 Slope_b=0;
					 write_config_in_nvs();
				}
				Aol*=Slope[4];
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ASW_DCA Aol=%lld\r\n",Aol);  
		}	
		else
		{
		    analog_switch(ASW_DCA);
		}
		
		if(Aol>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}
		
		Aol=labs(Aol);
		unit=0x05;
		measured_value=Aol;
		
	
}

//Measure AC current in mA
void measure_acma(uint32_t ad_dat)
{
	  int64_t Aol;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	/***Zero the AC current range***/
	 if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[4]=lsdat;
			  write_config_in_nvs();
		}
	
	  lsdat-=Zero[4];
	/***************/	
		
		Aol=lsdat*0.00095367; //Compute voltage in microvolts
        
		if(current_sw==ASW_ACMA)//Maximum 250 mA
		{
			 Aol*=0.35;
			  Aol*=Slope[3];
        //Aol*=1.116;
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ASW_MACA Aol=%lld\r\n",Aol);  
		}	
		else
		{
		    analog_switch(ASW_ACMA);
		}
		
		if(Aol>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}
		
		Aol=labs(Aol);
		unit=0x05;
		measured_value=Aol;
	
}

//Measure AC current in A
void measure_aca(uint32_t ad_dat)
{
	  int64_t Aol;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	/***Zero the AC current range***/
	 if(Zero_b==1)
		{
			  Zero_b=0;
				Zero[5]=lsdat;
			  write_config_in_nvs();
		}
	
	  lsdat-=Zero[5];
	/***************/	
		if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"lsdat=%d\r\n",lsdat);
		Aol=lsdat*0.00095367; //Compute voltage in microvolts

		if(current_sw==ASW_ACA)//Maximum 2.5 A
		{
			Aol*=6.7;
			  Aol*=Slope[4];
       // Aol*=1.116;
			  if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"ASW_ACA Aol=%lld\r\n",Aol);  
		}	
		else
		{
		    analog_switch(ASW_ACA);
		}
		
		if(Aol>=0)// The measured value is non-negative
		{
			sign=0;
		}
		else
		{
			sign=1;
		}
		
		Aol=labs(Aol);
		
		unit=0x05;
		measured_value=Aol;
	
}

//Measure resistance
void measure_r(uint32_t ad_dat)
{
	  uint64_t Vol;
	  uint64_t r=0;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	
	/***Zero the resistance range***/
	  if(Zero_b==1)
		{
			  Zero_b=0;
				if(lsdat<20000000)
				{
					Zero[6]=lsdat;
					write_config_in_nvs();
				}
		}
	
	  lsdat-=Zero[6];
	/***************/	
		if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"lsdat=%d\r\n",lsdat);

		if(lsdat>1200000000)
		{
		   analog_switch(ASW_R2);
		   sign=1;
			 unit=0x00;	
			 measured_value=0xFFFFFFFF;
		}
	  else if(lsdat<0)//Resistance-range voltage cannot be negative
		{
			 analog_switch(ASW_R5);
		   sign=1;
			 unit=0x00;	
			 measured_value=0;
		}
		else
		{ 
			  if(current_sw==ASW_R2)//1M
				{
				    Vol=lsdat*0.00095367; //Compute voltage in microvolts
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"1M Vol=%llu\r\n",Vol);
						r=((uint64_t)Vol*1000000000)/(1248000-Vol);//Compute resistance in milliohms
						if(Slope_b!=0)
						{ 
							Slope[5]=1000000000.0/r; 
							ESP_LOGI(ADC_TASK_TAG,"Slope[5]=%f\r\n",Slope[5]);
							Slope_b=0;
							write_config_in_nvs();
						}
						r*=Slope[5];
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"1M r=%llu\r\n",r);
					
					  if(r<200000000)//Switch range below 200 KOhm
						{
							  analog_switch(ASW_R3);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
				}
				else if(current_sw==ASW_R3)//100K
				{
				    Vol=lsdat*0.00095367; //Compute voltage in microvolts
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"100K Vol=%llu\r\n",Vol);
						r=((uint64_t)Vol*100000000)/(1248000-Vol);//Compute resistance in milliohms
						if(Slope_b!=0)
						{ 
							Slope[6]=100000000.0/r; 
							ESP_LOGI(ADC_TASK_TAG,"Slope[6]=%f\r\n",Slope[6]);
							Slope_b=0;
							write_config_in_nvs();
						}
						r*=Slope[6];
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"100K r=%llu\r\n",r);
					
					  if(r<20000000)//Switch range below 20 KOhm
						{
							  analog_switch(ASW_R4);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
						else if(r>250000000)//Switch range above 250 KOhm
						{
							  analog_switch(ASW_R2);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
				}
				else if(current_sw==ASW_R4)//10K
				{
				    Vol=lsdat*0.00095367; //Compute voltage in microvolts
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"10K Vol=%llu\r\n",Vol);
						r=((uint64_t)Vol*10000000)/(1248000-Vol);//Compute resistance in milliohms
						if(Slope_b!=0)
						{ 
							Slope[7]=10000000.0/r; 
							ESP_LOGI(ADC_TASK_TAG,"Slope[7]=%f\r\n",Slope[7]);
							Slope_b=0;
							write_config_in_nvs();
						}
						r*=Slope[7];
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"10K r=%llu\r\n",r);
					
					  if(r<2000000)//Switch range below 2 KOhm
						{
							  analog_switch(ASW_R5);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
						else if(r>25000000)//Switch range above 25 KOhm
						{
							  analog_switch(ASW_R3);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
				}	
				else if(current_sw==ASW_R5)//1K
				{
				    Vol=lsdat*0.00095367; //Compute voltage in microvolts
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"1K Vol=%llu\r\n",Vol);
					  r=((uint64_t)Vol*1000000)/(1248000-Vol);//Compute resistance in milliohms
					  if(Slope_b!=0)
						{ 
							Slope[8]=1000000.0/r; 
							ESP_LOGI(ADC_TASK_TAG,"Slope[8]=%f\r\n",Slope[8]);
							Slope_b=0;
							write_config_in_nvs();
						}
						r*=Slope[8];
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"1K r=%llu\r\n",r);
					
					  if(r>2500000)//Switch range above 2.5 KOhm
						{
							  analog_switch(ASW_R4);
							  vTaskDelay(pdMS_TO_TICKS(100));
							  //MCP3421_WriteReg(0x8C);
								vTaskDelay(pdMS_TO_TICKS(100));
								MCP3421_ReadReg();
						}
				}	
				else
				{
				    analog_switch(ASW_R2);
					  
				} 

				sign=0;
				
				if(r<1000000)//mR
				{
						unit=0x09;
						measured_value=r;
				}
				else if(r<5000000000)//R
				{
						unit=0x0A;
						measured_value=r/1000;
				}
				else//Overrange
				{
						unit=0x00;
						measured_value=0xFFFFFFFF;
				}
		}	
		
		
}




//Continuity buzzer range
void measure_beep(uint32_t ad_dat)
{
	  uint64_t Vol;
	  uint64_t r=0;
	  int lsdat=0;
	  static uint8_t beep_b=0;
	
		lsdat=((int)(ad_dat<<14));

	/***************/	
		if((lsdat>1200000000)|(lsdat<-500000000)) //Open circuit
		{
		   sign=1;
			 unit=0x00;	
			 measured_value=0xFFFFFFFF;
			if(beep_b==1)
			{
				beep_b=0;
				gpio_set_level(BEEP, 0);
			}
		}
	  else if(lsdat<0)//Resistance-range voltage cannot be negative
		{
			 analog_switch(ASW_BEEP);
		   //if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"Vol= - \r\n"); 
			 sign=1;
			 unit=0x00;
			 measured_value=0;
			 if(beep_b==1)
			{
				beep_b=0;
				gpio_set_level(BEEP, 0);
			}
		}
		else
		{ 
			  if(current_sw==ASW_BEEP)//1K
				{
				    Vol=lsdat*0.000058662; //Compute voltage in microvolts
						//if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"beep Vol=%llu\r\n",Vol);
					  r=((uint64_t)Vol*1000000)/(1248000-Vol);//Compute resistance in milliohms
					  r*=Slope[8];
						//if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"beep r=%llu\r\n",r);  
					
					  
				}	
				else
				{
				    analog_switch(ASW_BEEP);
					vTaskDelay(pdMS_TO_TICKS(100));
					MCP3421_ReadReg();
					r=0xFFFFFFFF;
					  
				}

        		sign=0;
				
				if(r<50000)//mR
				{
						unit=0x09;
						measured_value=r;
					  	if(beep_b==0)
						{
							beep_b=1;
							gpio_set_level(BEEP, 1);
						}
				}
				else 
				{
						unit=0x00;
						measured_value=0xFFFFFFFF;
					  	if(beep_b==1)
						{
							beep_b=0;
							gpio_set_level(BEEP, 0);
						}
				}
				
		}	
}


uint8_t power_add=0;
uint32_t power_Vol;
uint32_t power_Aol;
uint32_t power_value;

//Measure DC power
void measure_dcpower(uint32_t ad_dat)
{
	  uint64_t Vol;
		power_add++;
		if(power_add==1)
		{
		   measure_dca(ad_dat);
		}
		else if(power_add==2)
		{
		   measure_dca(ad_dat);
		}
		else if(power_add==3)
		{
		   measure_dca(ad_dat);
		}
		else if(power_add==4)
		{
		   measure_dca(ad_dat);
			 power_Aol=measured_value;
			 //ESP_LOGI(ADC_TASK_TAG,"power_Aol=%d\r\n",power_Aol);
		}
		else if(power_add==5)
		{
		   measure_dcv(ad_dat);
		}
		else if(power_add==6)
		{
		   measure_dcv(ad_dat);
		}
		else if(power_add==7)
		{
		   measure_dcv(ad_dat);
		}
		else if(power_add==8)
		{
		   measure_dcv(ad_dat);
			if(unit==0x01)
			{
			    power_Vol=measured_value/1000;
			}
			else if(unit==0x02)
			{
				  power_Vol=measured_value;
			}
		
			
			power_value=(power_Vol*power_Aol)/1000;
      ESP_LOGI(ADC_TASK_TAG,"power_value=%d\r\n",power_value);
			power_add=0;
		}

		unit=0x0C; //uW
		measured_value=power_value;

}

//Measure AC power
void measure_acpower(uint32_t ad_dat)
{
	  uint64_t Vol;
		power_add++;
		if(power_add==1)
		{
		   measure_aca(ad_dat);
		}
		else if(power_add==2)
		{
		   measure_aca(ad_dat);
		}
		else if(power_add==3)
		{
		   measure_aca(ad_dat);
		}
		else if(power_add==4)
		{
		   measure_aca(ad_dat);
			 power_Aol=measured_value;
			 //ESP_LOGI(ADC_TASK_TAG,"power_Aol=%d\r\n",power_Aol);
		}
		else if(power_add==5)
		{
		   measure_acv(ad_dat);
		}
		else if(power_add==6)
		{
		   measure_acv(ad_dat);
		}
		else if(power_add==7)
		{
		   measure_acv(ad_dat);
		}
		else if(power_add==8)
		{
		   measure_acv(ad_dat);
			if(unit==0x01)
			{
			    power_Vol=measured_value/1000;
			}
			else if(unit==0x02)
			{
				  power_Vol=measured_value;
			}
			 
			power_value=(power_Vol*power_Aol)/1000;
			ESP_LOGI(ADC_TASK_TAG,"power_value=%d\r\n",power_value);
			power_add=0;
		}

		unit=0x0C; //uW
		measured_value=power_value;

}




// Diode
void measure_diode(uint32_t ad_dat)
{
	  uint64_t Vol=0;
	  int lsdat=0;
	
		lsdat=((int)(ad_dat<<14));
	
	
		if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"lsdat=%d\r\n",lsdat);

		if(lsdat>1200000000)
		{
		   analog_switch(ASW_R5);
		   sign=1;
			 unit=0x00;	
			 measured_value=0xFFFFFFFF;
		}
	  else if(lsdat<0)//Diode voltage cannot be negative
		{
			 analog_switch(ASW_R5);
		   sign=1;
			 unit=0x00;	
			 measured_value=0;
		}
		else
		{ 
			  
				if(current_sw==ASW_R5)//1K
				{
				    Vol=lsdat*0.00095367; //Compute voltage in microvolts
						if(PRINTF_VALUE) ESP_LOGI(ADC_TASK_TAG,"diode Vol=%llu\r\n",Vol);
					  
				}	
				else
				{
				    analog_switch(ASW_R5);
					  
				} 

				sign=0;
				
				if(Vol<1200000)
				{
						unit=0x01;
						measured_value=Vol;
				}
				else//Overrange
				{
						unit=0x00;
						measured_value=0xFFFFFFFF;
				}
		}	
}



//Send measurements to the network periodically
esp_timer_handle_t esp_timer_handle_txdata = 0;
/* Timer interrupt callback */
void esp_timer_txdata_cb(void *arg){
    uint8_t evt;
	evt=WIFINET_MVOM;
	xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
	//ESP_LOGI(ADC_TASK_TAG, "TX DATA");
}


uint16_t pwroff_t_add=0;
//Poll battery level periodically
esp_timer_handle_t esp_timer_handle_batt = 0;
/* Timer interrupt callback */
void esp_timer_batt_cb(void *arg){
	uint8_t evt;
	int b_add=0;
 	 int val;
    esp_err_t r;
	int batt_v;

	val = (adc1_get_raw(ADC1_CHANNEL_1) & 0x1FFF);//Keep the lowest 13 bits
	batt_v = (2600.0/8191.0)*val*2;
	//ESP_LOGI(ADC_TASK_TAG, "batt_v = %d", batt_v);
	if(gpio_get_level(CHRG)==0) //Charging state
	{
		electricity_st=254;
	}
	else
	{
		if(batt_v<3200)
		{
			evt=WIFINET_MQTTSTOP;
            xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
			gpio_set_level(BEEP, 1);
			vTaskDelay(pdMS_TO_TICKS(100));
			gpio_set_level(BEEP, 0);
			vTaskDelay(pdMS_TO_TICKS(80));
			gpio_set_level(BEEP, 1);
			vTaskDelay(pdMS_TO_TICKS(100));
			gpio_set_level(BEEP, 0);
			gpio_set_level(PWR_EN, 0);
		}
		if(electricity_st==254)
		{
			if(batt_v<3300)
			{
				electricity_st=1;
			}
			else if(batt_v<3650)
			{
				electricity_st=25;
			}
			else if(batt_v<3750)
			{
				electricity_st=50;
			}
			else if(batt_v<3850)
			{
				electricity_st=75;
			}
			else 
			{
				electricity_st=100;
			}

			pwroff_t_add=600;
		}
		else
		{
			if(electricity_st==1)
			{
				if(batt_v>(3300+50))
				{
				electricity_st=25;
				}
				else
				{
				if(pwroff_t_add%60==0)//Low-battery alarm every minute
				{
					gpio_set_level(BEEP, 1);
					vTaskDelay(pdMS_TO_TICKS(100));
					gpio_set_level(BEEP, 0);
					vTaskDelay(pdMS_TO_TICKS(80));
					gpio_set_level(BEEP, 1);
					vTaskDelay(pdMS_TO_TICKS(100));
					gpio_set_level(BEEP, 0);
					vTaskDelay(pdMS_TO_TICKS(80));
					gpio_set_level(BEEP, 1);
					vTaskDelay(pdMS_TO_TICKS(100));
					gpio_set_level(BEEP, 0); 
				}
				if(--pwroff_t_add==0) //Power off after 10 minutes at low battery
				{
					evt=WIFINET_MQTTSTOP;
            		xQueueSendFromISR(wifinet_evt_queue, &evt, NULL);
					gpio_set_level(BEEP, 1);
					vTaskDelay(pdMS_TO_TICKS(100));
					gpio_set_level(BEEP, 0);
					vTaskDelay(pdMS_TO_TICKS(80));
					gpio_set_level(BEEP, 1);
					vTaskDelay(pdMS_TO_TICKS(100));
					gpio_set_level(BEEP, 0);
					gpio_set_level(PWR_EN, 0);
					
				}  
			}
		}
		else if(electricity_st==25)
		{
			if(batt_v>(3650+50))
			{
			electricity_st=50;
			}
			else if(batt_v<3350)
			{
			electricity_st=1;
			}
			pwroff_t_add=600;
		}
		else if(electricity_st==50)
		{
			if(batt_v>(3750+50))
			{
			electricity_st=75;
			}
			else if(batt_v<3650)
			{
			electricity_st=25;
			}
			pwroff_t_add=600;
		}
		else if(electricity_st==75)
		{
			if(batt_v>(3850+50))
			{
			electricity_st=100;
			}
			else if(batt_v<3750)
			{
			electricity_st=50;
			}
			pwroff_t_add=600;
		}
		else if(electricity_st==100)
		{
			if(batt_v<3850)
			{
			electricity_st=50;
			}
			pwroff_t_add=600;
		}
	} 
	}
}

void set_current_freq(){
    esp_timer_stop(esp_timer_handle_txdata);
    esp_timer_start_periodic(esp_timer_handle_txdata, current_freq * 100* 1000);
}



void adc_task(void *arg)
{
  uint32_t adc_value;
  int lsdat=0;
  long Vol;
  
   
   uint8_t evt;
  I2C_Init();

  adc1_config_width(ADC_WIDTH_BIT_13);//The RTC controller supports only 13 bits
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11); //Maximum range is 2.6 V




      // Initialize the timer structure
    esp_timer_create_args_t esp_timer_create_args_txdata = {
        .callback = &esp_timer_txdata_cb, // Timer callback function
        .arg = NULL, // Callback argument
        .name = "esp_timer_txdata" // Timer name
    };
   /* Create timer */
    esp_timer_create(&esp_timer_create_args_txdata, &esp_timer_handle_txdata);
    esp_timer_start_periodic(esp_timer_handle_txdata, current_freq * 100* 1000);


	 // Initialize the timer structure
    esp_timer_create_args_t esp_timer_create_args_batt = {
        .callback = &esp_timer_batt_cb, // Timer callback function
        .arg = NULL, // Callback argument
        .name = "esp_timer_batt" // Timer name
    };
   /* Create timer */
     esp_timer_create(&esp_timer_create_args_batt, &esp_timer_handle_batt);
    esp_timer_start_periodic(esp_timer_handle_batt, 200* 1000);


  if(current_fun==8)
  {
      MCP3421_WriteReg(0x94);//Continuity range: 60 samples per second
  }
  else
  {
      MCP3421_WriteReg(0x9C);//Other ranges: 3.75 samples per second
  }
	while (1) {



	if(current_fun_old!=current_fun)
	{
		current_fun_old=current_fun;
		if(current_fun==8)
		{
			vTaskDelay(pdMS_TO_TICKS(100));
			MCP3421_WriteReg(0x94);//60 samples per second
			ESP_LOGI(ADC_TASK_TAG,"qie1\r\n");
			
		}
		else
		{
			vTaskDelay(pdMS_TO_TICKS(100));
			//ESP_LOGI(ADC_TASK_TAG,"qie2\r\n");
			MCP3421_WriteReg(0x9C);//3.75 samples per second
		}
	}


    if(current_fun==8)
	{
		vTaskDelay(pdMS_TO_TICKS(50));
			adc_value = MCP3421_ReadReg();
			measure_beep(adc_value);	
	}					
	else 
	{
			vTaskDelay(pdMS_TO_TICKS(300));
			adc_value = MCP3421_ReadReg();
		    //ESP_LOGI(ADC_TASK_TAG,"ADC=%d\r\n",adc_value);
			switch(current_fun)
				{
					case 1:// DC voltage
						measure_dcv(adc_value);
					break;
					case 2:// AC voltage
						measure_acv(adc_value);
					break;
					case 3:// DC current in mA
						measure_dcma(adc_value);
					break;
					case 4:// DC current in A
						measure_dca(adc_value);
					break;
					case 5:// AC current in mA
						measure_acma(adc_value);
					break;
					case 6:// AC current in A
						measure_aca(adc_value);
					break;
					case 7:// Resistance
						measure_r(adc_value);
					break;
					case 8:// Continuity buzzer
						measure_beep(adc_value);
					break;
					case 9:// DC power
						measure_dcpower(adc_value);
					break;
					case 10:// AC power
						measure_acpower(adc_value);
					break;
					case 11:// Diode
						measure_diode(adc_value);
					break;
				} 
	}	
   }

}


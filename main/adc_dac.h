#ifndef _ADC_DAC_H
#define _ADC_DAC_H


extern uint8_t  Zero_b;  //Start zero calibration
extern uint8_t  Slope_b;

extern int Zero[10]; //Zero calibration values
extern float Slope[10];//Slope calibration
extern uint16_t current_freq;
extern uint8_t electricity_st; // Battery level: 0=low, 255=normal, 254=charging, 1-100=percentage

extern uint32_t measured_value ; //Measurement value; all Fs indicate overrange
extern uint8_t sign; //Sign; 1 indicates negative
extern uint8_t unit; //Measurement unit

void adc_task(void *arg);
void set_current_freq();

#endif

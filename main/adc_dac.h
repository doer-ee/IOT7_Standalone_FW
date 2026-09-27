#ifndef _ADC_DAC_H
#define _ADC_DAC_H
#include <stdbool.h>
#include <stdint.h>


extern uint8_t  Zero_b;  //Start zero calibration
extern uint8_t  Slope_b;

extern int Zero[10]; //Zero calibration values
extern float Slope[10];//Slope calibration
extern uint16_t current_freq;
extern uint8_t electricity_st; // Battery level: 0=low, 255=normal, 254=charging, 1-100=percentage

extern uint32_t measured_value ; //Measurement value; all Fs indicate overrange
extern uint8_t sign; //Sign; 1 indicates negative
extern uint8_t unit; //Measurement unit

typedef struct {
    uint32_t value_raw;
    uint32_t sequence;
    uint64_t timestamp_ms;
    uint8_t sign;
    uint8_t unit;
    uint8_t function;
    uint8_t range;
    uint8_t valid;
    uint8_t overrange;
} measurement_snapshot_t;

bool measurement_get_snapshot(measurement_snapshot_t *snapshot);
void measurement_invalidate_snapshot(void);
void adc_task(void *arg);
void set_current_freq();

#endif

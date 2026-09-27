#ifndef _SWITCH_FUN_H
#define _SWITCH_FUN_H



/* Range */
enum asw
{
    ASW_OFF  = 0,
    ASW_DCV0,
    ASW_DCV1,
    ASW_DCV2,
    ASW_DCV3,
    ASW_DCV4,
    ASW_DCV5,
    ASW_ACV0,
    ASW_ACV1,
    ASW_ACV2,
    ASW_ACV3,
    ASW_ACV4,
    ASW_ACV5,
    ASW_DCMA,
    ASW_DCA,
    ASW_ACMA,
    ASW_ACA,
    ASW_R0,
    ASW_R1,
    ASW_R2,
    ASW_R3,
    ASW_R4,
    ASW_R5,
    ASW_BEEP,
};

extern uint8_t current_sw;// Current range state
extern uint8_t current_fun; // Current function: 01 DC voltage, 02 AC voltage, 03 DC mA, 04 DC A, 05 AC mA, 06 AC A, 07 resistance, 08 continuity
extern uint8_t current_fun_old;

void switch_fun_task(void *arg);
void beep_start(uint8_t duration);
void analog_switch(uint8_t sw);

#endif

#ifndef IOT7_CONTROL_H
#define IOT7_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "adc_dac.h"

typedef enum {
    CONTROL_SET_FUNCTION = 0,
    CONTROL_SET_HOLD,
    CONTROL_ZERO,
    CONTROL_MARK,
    CONTROL_SET_RANGE,
} control_command_type_t;

typedef struct {
    control_command_type_t type;
    uint8_t function_id;
    uint8_t range;
    bool enabled;
    uint32_t request_id;
} control_command_t;

typedef struct {
    uint32_t request_id;
    uint32_t generation;
    uint8_t function_id;
    uint8_t range;
    bool hold;
    bool function_pending;
    bool range_pending;
    bool range_auto;
    bool zero_pending;
    bool last_command_ok;
    uint32_t last_mark_sequence;
    int last_error;
} control_state_t;

void control_init(void);

esp_err_t control_submit_function(uint8_t function_id, uint32_t *request_id);
esp_err_t control_submit_range(uint8_t range, uint32_t *request_id);
esp_err_t control_submit_hold(bool enabled, uint32_t *request_id);
esp_err_t control_submit_zero(uint32_t *request_id);
esp_err_t control_submit_mark(uint32_t *request_id);

bool control_get_state(control_state_t *state);
bool control_get_display_snapshot(measurement_snapshot_t *snapshot, bool *held);
bool control_zero_is_pending(void);
bool control_range_is_locked(uint8_t *range);
void control_notify_sample(uint8_t function_id, uint8_t range, bool valid, bool overrange,
                           bool range_ready);

#endif

#include "main.h"
#include "control.h"

#include <string.h>

static const char *CONTROL_TAG = "CONTROL";

#define CONTROL_QUEUE_LENGTH 8

static QueueHandle_t control_queue;
static portMUX_TYPE control_mux = portMUX_INITIALIZER_UNLOCKED;
static control_state_t control_state;
static measurement_snapshot_t held_snapshot;
static bool held_snapshot_valid;
static uint32_t next_request_id;

static uint32_t control_next_request_id(void)
{
    uint32_t request_id;
    portENTER_CRITICAL(&control_mux);
    request_id = ++next_request_id;
    if (request_id == 0) {
        request_id = ++next_request_id;
    }
    portEXIT_CRITICAL(&control_mux);
    return request_id;
}

static esp_err_t control_enqueue(control_command_t *command, uint32_t *request_id)
{
    if (control_queue == NULL || command == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    command->request_id = control_next_request_id();
    if (xQueueSend(control_queue, command, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (request_id != NULL) {
        *request_id = command->request_id;
    }
    return ESP_OK;
}

static void control_begin_command(const control_command_t *command)
{
    portENTER_CRITICAL(&control_mux);
    control_state.request_id = command->request_id;
    control_state.generation++;
    control_state.last_command_ok = true;
    control_state.last_error = 0;
    portEXIT_CRITICAL(&control_mux);
}

static void control_fail_command(int error)
{
    portENTER_CRITICAL(&control_mux);
    control_state.last_command_ok = false;
    control_state.last_error = error;
    portEXIT_CRITICAL(&control_mux);
}

static void control_apply_function(const control_command_t *command)
{
    measurement_snapshot_t snapshot;

    control_begin_command(command);
    current_fun = command->function_id;
    measurement_invalidate_snapshot();
    portENTER_CRITICAL(&control_mux);
    control_state.function_id = command->function_id;
    control_state.function_pending = true;
    control_state.range = current_sw;
    portEXIT_CRITICAL(&control_mux);

    if (measurement_get_snapshot(&snapshot)) {
        ESP_LOGI(CONTROL_TAG, "Function request %u applied; waiting for function %u snapshot",
                 (unsigned)command->request_id, (unsigned)command->function_id);
    } else {
        ESP_LOGI(CONTROL_TAG, "Function request %u applied; waiting for first snapshot",
                 (unsigned)command->request_id);
    }
}

static void control_apply_hold(const control_command_t *command)
{
    measurement_snapshot_t snapshot = {0};
    bool have_snapshot = measurement_get_snapshot(&snapshot);

    control_begin_command(command);
    portENTER_CRITICAL(&control_mux);
    control_state.hold = command->enabled;
    if (command->enabled && have_snapshot && snapshot.timestamp_ms != 0) {
        held_snapshot = snapshot;
        held_snapshot_valid = true;
    } else if (!command->enabled) {
        memset(&held_snapshot, 0, sizeof(held_snapshot));
        held_snapshot_valid = false;
    }
    portEXIT_CRITICAL(&control_mux);
    ESP_LOGI(CONTROL_TAG, "Measurement hold %s", command->enabled ? "enabled" : "disabled");
}

static void control_apply_zero(const control_command_t *command)
{
    control_begin_command(command);
    Zero_b = 1;
    portENTER_CRITICAL(&control_mux);
    control_state.zero_pending = true;
    portEXIT_CRITICAL(&control_mux);
    ESP_LOGI(CONTROL_TAG, "Zero request %u is pending", (unsigned)command->request_id);
}

static void control_apply_mark(const control_command_t *command)
{
    measurement_snapshot_t snapshot = {0};
    bool have_snapshot = measurement_get_snapshot(&snapshot);

    control_begin_command(command);
    if (!have_snapshot || snapshot.timestamp_ms == 0) {
        control_fail_command(ESP_ERR_INVALID_STATE);
        return;
    }

    portENTER_CRITICAL(&control_mux);
    control_state.last_mark_sequence = snapshot.sequence;
    portEXIT_CRITICAL(&control_mux);
    ESP_LOGI(CONTROL_TAG, "Marked sample sequence %u", (unsigned)snapshot.sequence);
}

static void control_task(void *arg)
{
    control_command_t command;
    (void)arg;

    while (true) {
        if (xQueueReceive(control_queue, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (command.type) {
            case CONTROL_SET_FUNCTION:
                control_apply_function(&command);
                break;
            case CONTROL_SET_HOLD:
                control_apply_hold(&command);
                break;
            case CONTROL_ZERO:
                control_apply_zero(&command);
                break;
            case CONTROL_MARK:
                control_apply_mark(&command);
                break;
            default:
                control_fail_command(ESP_ERR_INVALID_ARG);
                break;
        }
    }
}

void control_init(void)
{
    if (control_queue != NULL) {
        return;
    }
    control_queue = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_command_t));
    if (control_queue == NULL) {
        ESP_LOGE(CONTROL_TAG, "Failed to create control queue");
        return;
    }
    memset(&control_state, 0, sizeof(control_state));
    control_state.function_id = current_fun;
    control_state.range = current_sw;
    control_state.last_command_ok = true;
    xTaskCreate(control_task, "CONTROL", 1024 * 3, NULL, 8, NULL);
}

esp_err_t control_submit_function(uint8_t function_id, uint32_t *request_id)
{
    if (function_id < 1 || function_id > 11) {
        return ESP_ERR_INVALID_ARG;
    }
    control_command_t command = {
        .type = CONTROL_SET_FUNCTION,
        .function_id = function_id,
    };
    return control_enqueue(&command, request_id);
}

esp_err_t control_submit_hold(bool enabled, uint32_t *request_id)
{
    control_command_t command = {
        .type = CONTROL_SET_HOLD,
        .enabled = enabled,
    };
    return control_enqueue(&command, request_id);
}

esp_err_t control_submit_zero(uint32_t *request_id)
{
    measurement_snapshot_t snapshot = {0};
    if (!measurement_get_snapshot(&snapshot) || !snapshot.valid || snapshot.overrange) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&control_mux);
    bool pending = control_state.zero_pending;
    portEXIT_CRITICAL(&control_mux);
    if (pending || Zero_b != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    control_command_t command = {
        .type = CONTROL_ZERO,
    };
    return control_enqueue(&command, request_id);
}

esp_err_t control_submit_mark(uint32_t *request_id)
{
    measurement_snapshot_t snapshot = {0};
    if (!measurement_get_snapshot(&snapshot) || snapshot.timestamp_ms == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    control_command_t command = {
        .type = CONTROL_MARK,
    };
    return control_enqueue(&command, request_id);
}

bool control_get_state(control_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    portENTER_CRITICAL(&control_mux);
    *state = control_state;
    state->function_id = current_fun;
    state->range = current_sw;
    portEXIT_CRITICAL(&control_mux);
    return true;
}

bool control_get_display_snapshot(measurement_snapshot_t *snapshot, bool *held)
{
    if (snapshot == NULL) {
        return false;
    }
    bool use_hold;
    bool have_held;
    measurement_snapshot_t held_copy;
    portENTER_CRITICAL(&control_mux);
    use_hold = control_state.hold;
    have_held = held_snapshot_valid;
    held_copy = held_snapshot;
    portEXIT_CRITICAL(&control_mux);

    if (held != NULL) {
        *held = use_hold;
    }
    if (use_hold && have_held) {
        *snapshot = held_copy;
        return true;
    }
    return measurement_get_snapshot(snapshot);
}

bool control_zero_is_pending(void)
{
    bool pending;
    portENTER_CRITICAL(&control_mux);
    pending = control_state.zero_pending;
    portEXIT_CRITICAL(&control_mux);
    return pending;
}

void control_notify_sample(uint8_t function_id, uint8_t range, bool valid, bool overrange,
                           bool range_ready)
{
    portENTER_CRITICAL(&control_mux);
    if (range_ready && control_state.function_pending && control_state.function_id == function_id) {
        control_state.function_pending = false;
        control_state.range = range;
    }
    if (control_state.zero_pending) {
        control_state.zero_pending = false;
        control_state.last_command_ok = valid && !overrange;
        control_state.last_error = control_state.last_command_ok ? 0 : ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&control_mux);
}

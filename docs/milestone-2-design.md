# Milestone 2 Design: Web Controls

> Historical design record. The standalone cleanup described in [standalone-firmware.md](standalone-firmware.md) removed MQTT and the Report period API after this milestone. Use the standalone document for current behavior.

## Goal

Milestone 2 adds local-network control of the existing measurement functions while keeping the original MQTT and maintenance UART behavior. The web server will submit commands to a dedicated control queue. It will not write `current_fun`, `current_freq`, calibration flags, or GPIO outputs from an HTTP handler.

The first implementation remains available only on the station IP after the device has joined the configured Wi-Fi network. The provisioning SoftAP keeps its current Wi-Fi configuration page. SoftAP control access, authentication, and remote access are later concerns.

## Existing behavior that affects the design

- `current_fun` is read by the ADC task and currently written by the UART parser, MQTT callback, and legacy paths. Those writers must be routed through one control API.
- `current_sw` and the analog-switch GPIOs are managed by the measurement path. Web code must request a function change and let the ADC task settle the range.
- `current_freq` is stored in units of 100 ms and currently controls the MQTT/report timer through `esp_timer_handle_txdata`. It does not change the MCP3421 conversion interval or the browser polling interval. The API will therefore call this `report_period_ms`, not `sample_period_ms`.
- `Zero_b` is consumed by the next measurement conversion and the resulting calibration is written to NVS. Zeroing must be a queued operation with a visible pending/result state.
- The existing mark event sends the legacy MQTT `0x080B` packet. The web mark command should preserve that behavior when MQTT is connected and also return the local sample sequence used for the mark.

## Control command model

Add a small control module or equivalent code in `main/` with these concepts:

```c
typedef enum {
    CONTROL_SET_FUNCTION,
    CONTROL_SET_REPORT_PERIOD,
    CONTROL_SET_HOLD,
    CONTROL_ZERO,
    CONTROL_MARK,
} control_command_type_t;

typedef struct {
    control_command_type_t type;
    uint8_t function_id;
    uint16_t report_period_100ms;
    bool enabled;
    uint32_t request_id;
} control_command_t;
```

The queue depth should be 8 or more. A control task is the only task that applies command-side changes. It updates a small control-state snapshot guarded by a critical section or mutex:

```c
typedef struct {
    uint32_t request_id;
    uint32_t generation;
    uint8_t function_id;
    uint8_t range;
    uint16_t report_period_100ms;
    bool hold;
    bool function_pending;
    bool zero_pending;
    bool last_command_ok;
    uint32_t last_mark_sequence;
    int last_error;
} control_state_t;
```

The HTTP, UART, and MQTT entry points call `control_submit()`. They do not assign the legacy globals directly. The control task may update the existing globals because it is the single command writer; the ADC task remains responsible for range switching and measurement snapshots.

## HTTP API

The request bodies use `application/x-www-form-urlencoded` to match the existing ESP-IDF form parser and avoid adding a JSON parser in this milestone. Every successful submission returns HTTP `202 Accepted` with a JSON request ID. Invalid values return HTTP `400`; a full command queue returns HTTP `503`.

### `GET /api/control`

Returns the current control state and the active measurement function. Example:

```json
{
  "function_id": 7,
  "function": "Resistance",
  "range": 21,
  "range_label": "R4",
  "report_period_ms": 500,
  "hold": false,
  "function_pending": false,
  "zero_pending": false,
  "last_command_ok": true,
  "last_mark_sequence": 0,
  "request_id": 12,
  "generation": 4,
  "last_error": 0
}
```

### `POST /api/control/function`

Body: `function=dcv`, `acv`, `dcma`, `dca`, `acma`, `aca`, `resistance`, `continuity`, `dc_power`, `ac_power`, or `diode`.

The control task validates the function, updates `current_fun`, clears the old measurement snapshot, and lets the ADC task select and settle the corresponding range. The response remains pending until a snapshot with the new function and range is published.

### `POST /api/control/period`

Body: `period_ms=200` through `period_ms=6000000`, in 100 ms steps. This changes the existing MQTT/report timer. It does not claim to change ADC conversion speed.

The accepted value is converted to the legacy `current_freq` units and persisted only when it differs from the current value. Repeated identical submissions must not write NVS.

### `POST /api/control/hold`

Body: `enabled=1` or `enabled=0`.

Hold is a presentation/output state. The ADC task continues sampling and automatic range switching, while the Web UI displays the last held snapshot and reports that newer samples are available. Releasing hold immediately exposes the newest coherent snapshot.

### `POST /api/control/zero`

The request has no measurement payload. The UI must show a warning and ask the user to remove external input before submitting. The control task accepts one pending zero operation, sets `Zero_b`, and reports completion or failure after the next applicable conversion.

Zero commands are not silently retried, and the API must expose whether the pending operation was applied. This protects against repeated browser clicks and makes calibration changes auditable in the serial log.

### `POST /api/control/mark`

Captures the current coherent sample sequence and timestamp. It also submits the existing MQTT mark event when the legacy MQTT connection is available. It does not write the mark to flash.

## Web UI changes

Add a `Controls` section below the live measurement card:

1. Function selector with the supported function names and a visible pending/settling state.
2. Report period input in milliseconds, with the current value and validation feedback.
3. Hold toggle showing whether the displayed value is frozen.
4. Zero button with a confirmation dialog and a `zero_pending` result message.
5. Mark button showing the marked sample sequence and local timestamp.
6. One-second polling of `/api/control`; the existing measurement polling remains unchanged.

Controls are disabled while the device is disconnected or while a function transition is pending. The UI uses `fetch()` and form-encoded bodies, with no WebSocket dependency.

## Command flow

```text
HTTP / UART / MQTT input
        |
        v
validate and enqueue control_command_t
        |
        v
control task applies one command
        |
        +--> function: update current_fun, clear snapshot
        +--> period: update current_freq, restart report timer, persist
        +--> hold: update output state
        +--> zero: set Zero_b, await next conversion
        +--> mark: capture sequence, send legacy MQTT mark
        |
        v
ADC task settles range and publishes a coherent snapshot
```

## Safety and compatibility

- Web control is local-network only in this milestone and has no authentication. This limitation must be visible in the UI and documentation.
- Function switching can change the analog front-end while probes are connected. The UI must show a short transition state; later revisions can add an explicit high-energy input warning.
- Zero and slope calibration remain separate. Web control exposes zero first; slope calibration stays maintenance-only until a safer calibration workflow exists.
- Existing UART commands and MQTT function-setting packets must call the same control submission functions. This prevents the three control paths from drifting.
- Do not alter the original MQTT packet formats in this milestone.
- Avoid NVS writes for display-only hold and mark state. Persist only the report period and the existing zero result.

## Implementation order

1. Add the control command/state types, queue, and `GET /api/control`.
2. Route existing UART and MQTT function/period/zero/mark writers through the control API.
3. Add the five POST handlers with strict validation and bounded request bodies.
4. Add the Controls section to the English Web UI.
5. Build and test the queue under two browser tabs, then flash only after the build and backup checks pass.
6. Validate function switching, hold behavior, report-period behavior, zeroing with no input, marking, and legacy UART/MQTT compatibility.

## Acceptance criteria

- A malformed or unsupported command returns `400` and changes no measurement state.
- A valid function request returns `202`, reaches the control task once, clears the old snapshot, and becomes ready only after the new function/range snapshot is published.
- Hold freezes the displayed sample while the underlying sample sequence continues.
- Report-period changes affect the legacy report timer only and survive reboot.
- Zero requires an explicit UI confirmation, is applied once, reports its result, and does not run on an overrange sample.
- Mark returns the sample sequence and preserves the legacy MQTT mark event when connected.
- UART and MQTT function changes still work through the same queue.
- No HTTP handler directly calls `gpio_set_level`, `analog_switch`, or writes the command globals.

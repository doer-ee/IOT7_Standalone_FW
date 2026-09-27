# Milestone 1: Read-only measurement

## Scope

The firmware publishes one coherent measurement snapshot after each completed ADC conversion. The local Web UI polls `GET /api/measurement` once per second and `GET /api/status` every three seconds. It shows the raw reading, function, active range, battery state, Wi-Fi signal, sample age, and the interval between samples observed by the browser. No Web control endpoint is included in this milestone.

An ADC read error invalidates the snapshot. A conversion that is not ready does not advance the sequence. After a function or automatic range change, the page waits for a sample from the new setting. Power modes publish only after a complete current/voltage cycle.

## API

Example shape (values are illustrative; the actual unit code and scaling still need hardware verification):

```json
{
  "sample_ready": true,
  "range_switching": false,
  "valid": true,
  "overrange": false,
  "function": "DC voltage",
  "function_id": 1,
  "range": 4,
  "range_label": "DCV3",
  "value_raw": 123456,
  "sign": 0,
  "unit_code": 1,
  "unit": "uV",
  "sample_sequence": 42,
  "timestamp_ms": 1234567,
  "sample_age_ms": 120,
  "battery": 100
}
```

`timestamp_ms` is monotonic time since boot, not wall-clock time. `value_raw` is the existing firmware's integer representation. The page deliberately displays this integer with the unit code's label; it does not silently rescale it. `valid=false` with `sample_ready=true` means the firmware produced an invalid reading. `overrange=true` means the original measurement path returned `0xFFFFFFFF`.

Source-inferred unit codes:

| Code | Label | Description |
| --- | --- | --- |
| `0x01` | `uV` | microvolts |
| `0x02` | `mV` | millivolts |
| `0x05` | `uA` | microamps |
| `0x09` | `mOhm` | milliohms |
| `0x0A` | `Ohm` | ohms |
| `0x0C` | `uW` | microwatts |

`battery` retains the firmware's existing encoding: `0` is low, `1-100` is a percentage, `254` is charging, and `255` is normal without a percentage. Range labels identify firmware switch states, not certified voltage limits.

## Hardware verification

1. Flash the built application through the established backup and recovery procedure, then boot with the battery and confirm Wi-Fi connection. Open `http://<device-ip>/` and `http://<device-ip>/api/measurement`.
2. Use the three-pad UART maintenance command `fun=dcv` to select DC voltage. Compare a known low-voltage source against a trusted meter. Repeat with reversed polarity. Record `value_raw`, `sign`, `unit_code`, `range`, `sample_sequence`, and the reference value.
3. Remove the voltage source. Select resistance with `fun=r`; compare a known resistor against the reference meter, then open the probes to check overrange. Do not send `zero` or `slope` during this verification because those commands change calibration.
4. Keep the page open for at least one minute. Confirm that the sequence advances only on completed conversions, the sample age resets on a new sample, and the observed interval is plausible for the selected ADC mode. Disconnect and reconnect Wi-Fi to check the page's connection state.
5. Record the results below before treating unit scaling, polarity, overrange, and refresh timing as verified.

| Test | Reference | Raw + unit | Sign | Range | Result |
| --- | --- | --- | --- | --- | --- |
| DC voltage, positive | 2.611 V | approximately 2.610662 V, `uV` representation | 0 | `DCV3` | Pass; approximately -0.016% |
| DC voltage, reversed | 2.611 V magnitude | approximately 2.611 V, `uV` representation | 1 consistently | `DCV3` | Pass; polarity flag verified |
| Known resistor | 9.91 kΩ | 9940–9941 Ω, mean 9940.12 Ω | 0 | `R4` | Pass; approximately 0.30% from nominal |
| Open probes | Open circuit | `0xFFFFFFFF`, no unit | 1 while invalid | `R2` | Pass; 12/12 samples had `valid=false` and `overrange=true` |
| One-minute refresh | 62.8 s API poll | Sequence 4020→4226; timestamp advanced 61,810 ms; sample age 3–300 ms | n/a | `R2` open probes | Pass; 54 samples, monotonic sequence, no request errors |

## Build

Use the existing ESP-IDF v4.4.3 environment, then run `idf.py build` from the repository root. The application image is `build/esp32s2nolcd.bin`. Compilation verifies the code path but does not verify live measurements or calibration.

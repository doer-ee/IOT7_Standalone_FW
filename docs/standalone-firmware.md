# Standalone firmware architecture

## Current network behavior

- A normal boot loads the router SSID and password from the `wificonfig` NVS namespace and starts Wi-Fi station mode.
- If no SSID is stored, the device starts the `Doer.ee-Setup-<device ID suffix>` Wi-Fi configuration AP. Holding the power button for about three seconds during startup also opens this AP. The configuration page is at `http://192.168.4.1/`.
- Once the station receives an IP address, the local Web UI is available at `http://<device IP>/`. The device also initializes SNTP for its system clock.
- MQTT, SmartConfig/AirKiss, optical screen provisioning, and the placeholder cloud OTA client have been removed from the application. The ESP32-S2 ROM USB download mode remains available for firmware recovery.

## Local interfaces

- `GET /api/status`: Wi-Fi and device status.
- `GET /api/measurement`: coherent measurement snapshot and battery state.
- `GET /api/control`: queued control state.
- `POST /api/control/function`, `/range`, `/hold`, `/zero`, and `/mark`: local measurement controls. Voltage and resistance support fixed range locks; current range buttons select the existing mA or A hardware path.
- `GET /api/settings`: current non-secret Wi-Fi and mDNS settings.
- `POST /api/settings/wifi` and `/api/settings/mdns`: update local Wi-Fi and mDNS settings.
- UART0 maintenance commands remain available, including function selection, zeroing, slope calibration, and device ID updates.

The old `POST /api/control/period` endpoint and Report period control were removed because they only adjusted the deleted MQTT publishing timer. The ADC conversion rate and Web UI polling interval were never controlled by that setting.

## Persistent data and recovery

The cleanup does not erase NVS. The router credentials, calibration values, selected function, and device ID still use their existing keys. The obsolete `parameter/password` and `parameter/c_freq` keys are left in flash but are no longer read or written.

The 4 MiB flash layout reserves two 1 MiB OTA application slots and a separate SPIFFS partition for future measurement records:

| Partition | Offset | Size | Purpose |
| --- | ---: | ---: | --- |
| `ota_0` | `0x10000` | `0x100000` | OTA application slot 0 |
| `ota_1` | `0x110000` | `0x100000` | OTA application slot 1 |
| `data_collection` | `0x210000` | `0x1F0000` | Future measurement storage |

The current application image is below 1 MiB. The data partition is reserved by the partition table but is not mounted or written by the current firmware yet. Changing to this layout moves `ota_1` from `0x200000` to `0x110000`; the first layout migration must therefore flash the new partition table and an application image at the new slot address instead of writing only the old OTA1 address.

## Validation after flashing

Check battery boot, station Wi-Fi, the status and measurement APIs, all local controls, the startup Web configuration AP, UART maintenance, and continuity response. A successful build alone does not verify hardware behavior.

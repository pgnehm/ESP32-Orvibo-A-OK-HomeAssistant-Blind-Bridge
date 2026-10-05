# Home Assistant exposed surface

The bridge uses MQTT discovery. Home Assistant entity IDs may differ if you rename entities in the UI, but the `unique_id` values and MQTT topics below are stable.

## Bridge availability

Topic:

```text
orvibo_esp32_blinds/status
```

Values:

- `online`
- `offline`

The firmware publishes `online` after MQTT connection. MQTT last-will publishes `offline` if the bridge disconnects unexpectedly.

## Per-blind cover entity

Discovery topic:

```text
homeassistant/cover/orvibo_esp32_blinds/<blind_id>/config
```

Default example entity:

```text
cover.living_room_blind
```

MQTT topics:

| Purpose | Topic | Payload |
| --- | --- | --- |
| Cover command | `orvibo_esp32_blinds/<blind_id>/set` | `OPEN`, `CLOSE`, `STOP`, `SYNC_OPEN`, `SYNC_CLOSE`, `STATUS`, or `0-100` |
| Position command | `orvibo_esp32_blinds/<blind_id>/set_position` | `0-100` |
| Cover state | `orvibo_esp32_blinds/<blind_id>/state` | `open`, `opening`, `closed`, `closing`, `stopped` |
| Position state | `orvibo_esp32_blinds/<blind_id>/position` | `0-100` |
| JSON attributes | `orvibo_esp32_blinds/<blind_id>/attributes` | JSON object |

Cover features:

- Open
- Close
- Stop
- Set position

The discovery payload sets `optimistic: true`, which makes Home Assistant expose
the cover as `assumed_state: true`. This is intentional: the motors do not send
position feedback, so Open, Close, and Stop must remain available even when the
timing estimate says the blind is fully open or fully closed. State and position
topics are still published for motion display, diagnostics, and automations.

Cover attributes:

| Attribute | Meaning |
| --- | --- |
| `id` | Blind ID from `include/blinds.local.h` or `include/blinds.example.h`. |
| `motion` | Current motion estimate: `opening`, `closing`, or `stopped`. |
| `position_known` | `true` if the bridge has a usable position estimate. |
| `estimated_position` | Timing-based position estimate, `0-100`, or `null`. |
| `target_position` | Current requested target position, `0-100`, or `null`. |
| `full_travel_ms` | Configured full travel time for this blind. |
| `invert_position` | Whether this blind reverses open/closed position semantics. |
| `rf_frequency_mhz` | Configured CC1101 transmit frequency. |
| `rf_open_configured` | Whether an open pulse train exists. |
| `rf_close_configured` | Whether a close pulse train exists. |
| `rf_stop_configured` | Whether a stop pulse train exists. |
| `last_command` | Last accepted, rejected, or automatic command label. |
| `last_result` | Last command result: `idle`, `sent`, `failed`, `ignored`, `synced`, `unchanged`, or `completed`. |
| `last_error` | Last command error message, or empty string. |
| `command_count` | Number of commands handled by the firmware for this blind. |
| `failed_command_count` | Number of failed or ignored commands for this blind. |
| `last_command_ms` | `millis()` timestamp of the last command, or `null`. |

Removed covers can be cleaned out of Home Assistant by defining
`MQTT_RETIRED_COVER_IDS` in `include/config.local.h`. The bridge publishes an empty
retained discovery payload for each listed ID on MQTT connection, then clears the
old retained state and command topics.

## Bridge diagnostic entities

All diagnostics use the same bridge availability topic.

| Entity kind | Name | Unique ID | State topic | Payload |
| --- | --- | --- | --- | --- |
| Sensor | WiFi RSSI | `orvibo_esp32_blinds_wifi_rssi` | `orvibo_esp32_blinds/diagnostics/wifi_rssi` | dBm |
| Sensor | Uptime | `orvibo_esp32_blinds_uptime` | `orvibo_esp32_blinds/diagnostics/uptime` | seconds |
| Sensor | IP Address | `orvibo_esp32_blinds_ip_address` | `orvibo_esp32_blinds/diagnostics/ip_address` | IP string |
| Sensor | Free Heap | `orvibo_esp32_blinds_free_heap` | `orvibo_esp32_blinds/diagnostics/free_heap` | bytes |
| Sensor | Firmware Version | `orvibo_esp32_blinds_firmware_version` | `orvibo_esp32_blinds/diagnostics/firmware_version` | version string |
| Sensor | LED Status | `orvibo_esp32_blinds_led_status` | `orvibo_esp32_blinds/diagnostics/led_status` | status string |
| Sensor | Debug URL | `orvibo_esp32_blinds_debug_url` | `orvibo_esp32_blinds/diagnostics/debug_url` | HTTP URL |
| Sensor | Last Log | `orvibo_esp32_blinds_last_log` | `orvibo_esp32_blinds/diagnostics/last_log` | latest bounded log line |
| Sensor | Log Sequence | `orvibo_esp32_blinds_log_sequence` | `orvibo_esp32_blinds/diagnostics/log_sequence` | counter |
| Sensor | Dropped Logs | `orvibo_esp32_blinds_log_dropped` | `orvibo_esp32_blinds/diagnostics/log_dropped` | counter |
| Binary sensor | CC1101 Radio | `orvibo_esp32_blinds_radio_ready` | `orvibo_esp32_blinds/diagnostics/radio_ready` | `ready` or `missing` |

## Position behavior

The bridge does not receive physical position feedback from the blind motor. It estimates position from configured full-travel time.

Important behavior:

- Open, Close, and Stop remain available regardless of the estimated position.
- Full open and full close can be sent even when position is unknown.
- Intermediate positions require a known current position.
- Intermediate positions also require a configured stop RF command.
- After firmware reboot, send `SYNC_OPEN` or `SYNC_CLOSE` if the blind is already at a known endpoint and you do not want to transmit RF.

## Local HTTP debug surface

The bridge also runs a local HTTP debug server after WiFi connects. It works independently of MQTT.

| Endpoint | Purpose |
| --- | --- |
| `/` | Status page, LED legend, RF capture form, recent logs, and reboot button |
| `/status` | JSON status, including `time`, `time_synced`, WiFi, MQTT, radio, LED, logs, and blinds |
| `/logs` | Plain text bounded log with local date/time after NTP sync |
| `/logs.json` | JSON bounded log entries |
| `/capture` | Browser RF capture page |
| `/capture.raw` | Last RF capture as raw pulse text |
| `/capture.json` | Last RF capture as JSON |
| `/reboot` | Reboot confirmation page |
| `POST /update` | Hidden HTTP firmware upload endpoint |

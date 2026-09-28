# ESP32 Orvibo A-OK Home Assistant Blind Bridge

ESP32 + CC1101 firmware for controlling A-OK 433 MHz tubular blind motors from Home Assistant.

This project was built for an installation that originally used an ORVIBO Allone Pro controller and the ORVIBO Home app. The observed ORVIBO controller is an Allone Pro multifunction smart host, model `VS20RB-1GO`, and the blind motor identified during the project is an A-OK multi-point tubular motor, model `AM35-6/28-MEL-ZG`.

Instead of depending on the ORVIBO cloud or app, the ESP32 transmits the same style of RF commands that the physical A-OK remotes send. Home Assistant controls the ESP32 through MQTT discovery and sees each blind as a normal `cover` entity.
<img width="635" height="665" alt="Screenshot 2026-09-17 095729" src="https://github.com/user-attachments/assets/1377069f-8354-4b25-8516-4c9622689131" />

## What It Can Do

- Control one blind, several individual blinds, or grouped blinds from Home Assistant.
- Expose each configured blind as a Home Assistant MQTT `cover`.
- Support `OPEN`, `CLOSE`, `STOP`, set-position commands, and endpoint sync commands.
- Estimate blind position from configured travel time.
- Replay learned A-OK RF commands through a CC1101 radio at `433.92 MHz`.
- Generate per-blind command tables from captured A-OK remote calibration.
- Support multiple remotes/calibrations in one build.
- Provide a local web interface for status, logs, reboot, and RF capture.
- Capture remote button presses over WiFi when the ESP32 is no longer connected by USB.
- Keep a bounded remote log so debugging does not exhaust memory.
- Add readable date/time to log events after NTP sync.
- Publish diagnostics to Home Assistant, including IP, uptime, RSSI, heap, firmware version, LED status, and log state.
- Show firmware state on the onboard RGB LED.
- Allow firmware upload over WiFi after the first USB flash.

## Web Interface

<img width="371" height="703" alt="Screenshot 2026-09-17 095701" src="https://github.com/user-attachments/assets/f21df13f-c553-4359-b853-53ac8e107e3e" />


## Important Limitations

- The motor does not report real position back to the ESP32. Position is an estimate based on travel time.
- Intermediate position moves require a known current position and a working stop command.
- RF replay depends on the remote protocol being reproducible. If a motor or remote uses true rolling code/encryption, direct replay may fail.
- The hidden HTTP firmware upload endpoint is not authenticated. Use it only on a trusted LAN or add network-level protection.
- Raw captures and real calibration JSON can control your blinds. They are intentionally ignored by git.

## Hardware

Required:

- ESP32-S3 development board. This project was tested on an ESP32-S3 DevKitC-1 style N16R8 screw-terminal board.
- CC1101 433 MHz transceiver module.
- 433 MHz antenna for the CC1101.
- Jumper wires.
- 3.3V power from the ESP32 to the CC1101.

Optional:

- RTL-SDR receiver for RF inspection. It is helpful, but not required.
- External WiFi antenna for the ESP32-S3 board if the board is configured for external antenna use.

Do not power the CC1101 from 5V. Use 3.3V only.

<img width="272" height="469" alt="image" src="https://github.com/user-attachments/assets/0f961f73-2cd1-421b-8598-9404b29c3e7e" />


## Wiring

The public example config targets the ESP32-S3 N16R8 screw-terminal board used during development.

| CC1101 pin | CC1101 label | ESP32-S3 GPIO | Notes |
| --- | --- | --- | --- |
| 1 | GND | GND | Shared ground |
| 2 | VCC | 3.3V | Do not use 5V |
| 3 | GDO0 | GPIO14 | RF data input/output |
| 4 | CSN | GPIO10 | SPI chip select |
| 5 | SCK | GPIO12 | SPI clock |
| 6 | MOSI | GPIO11 | SPI data to CC1101 |
| 7 | MISO/GDO1 | GPIO13 | SPI data from CC1101 |
| 8 | GDO2 | Not connected | Unused |

If your ESP32 board uses different safe GPIOs, edit these values in `include/config.local.h`:

```cpp
#define CC1101_SCK_PIN 12
#define CC1101_MISO_PIN 13
#define CC1101_MOSI_PIN 11
#define CC1101_CS_PIN 10
#define RF_TX_PIN 14
#define RF_RX_PIN RF_TX_PIN
```

The onboard WS2812 RGB LED is configured with `STATUS_RGB_PIN`. The tested ESP32-S3 N16R8 board uses GPIO48.

## RGB LED Status

The web interface includes this table, and the firmware uses the same states:

| RGB LED | Meaning |
| --- | --- |
| Dim blue | Starting up |
| Amber blinking | Connecting to WiFi |
| Blue blinking | WiFi connected, connecting to Home Assistant/MQTT |
| Solid green | Ready: WiFi, MQTT, and CC1101 radio are working |
| White | Sending a blind RF command |
| Yellow blinking | Listening for one physical remote button during RF capture |
| Cyan blinking | Firmware upload in progress |
| Purple | Reboot requested |
| Red blinking or solid red | CC1101 radio missing or another error needs attention |

The small TX/RX LEDs on the tested board are driven by the USB-to-serial chip. They are not general ESP32 status outputs in this firmware.

## Repository Layout

```text
src/main.cpp                         Bridge firmware
src/capture_main.cpp                 Serial RF capture firmware
include/config.example.h             Public config template
include/config.h                     Loads config.local.h when present
include/blinds.example.h             Compile-safe blind placeholder
include/blinds.h                     Loads blinds.local.h when present
include/blind_types.h                Shared blind and RF command structs
data/blinds.example.json             Example blind/channel mapping
data/remote_calibration.example.json Example A-OK remote calibration
tools/aok_protocol.py                A-OK decode/encode helpers
tools/decode_capture.py              Decode raw pulse captures
tools/make_calibration.py            Build calibration JSON from decoded payloads
tools/generate_blinds_local.py       Generate include/blinds.local.h
captures/capture-log-template.md     Capture worksheet
docs/home-assistant-surface.md       MQTT and HA entity details
docs/rf-capture-plan.md              RF capture workflow
docs/integration-source-notes.md     ORVIBO/HomeMate research notes
```

Ignored local/private files:

- `include/config.local.h`
- `include/blinds.local.h`
- `data/blinds.json`
- `data/*calibration*.json`, except `data/remote_calibration.example.json`
- raw files under `captures/`

## Setup

Install PlatformIO first. From this repository:

```powershell
python -m platformio run -e esp32dev
```

Create a private config:

```powershell
Copy-Item .\include\config.example.h .\include\config.local.h
```

Edit `include/config.local.h`:

```cpp
#define WIFI_SSID "YOUR_WIFI"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USERNAME "mqttuser"
#define MQTT_PASSWORD "YOUR_MQTT_PASSWORD"
```

The defaults assume:

- MQTT discovery prefix: `homeassistant`
- MQTT base topic: `orvibo_esp32_blinds`
- RF frequency: `433.92 MHz`
- NTP timezone: US Eastern, `EST5EDT,M3.2.0/2,M11.1.0/2`

## Build And Upload

Build:

```powershell
python -m platformio run -e esp32dev
```

Upload over USB:

```powershell
python -m platformio run -e esp32dev -t upload
```

Open serial monitor:

```powershell
python -m platformio device monitor -e esp32dev
```

After a firmware with WiFi update support is already installed, you can upload with the hidden HTTP updater:

```powershell
curl.exe -f -S -F "firmware=@.pio\build\esp32dev\firmware.bin;type=application/octet-stream" http://<esp32-ip>/update
```

ArduinoOTA is also enabled after WiFi connects:

```powershell
python -m platformio run -e esp32dev_ota -t upload
```

The OTA environment defaults to `orvibo_esp32_blinds.local`. If mDNS does not work on your network, pass or edit the ESP32 IP address.

## Capturing A-OK Remote Commands

The bridge can capture over WiFi, and there is also a dedicated serial capture firmware.

Use normal up/down/stop buttons only. Do not long-press program or pair while learning the protocol.

### Option 1: WiFi Capture

Use this when the ESP32 is already installed away from the PC.

1. Open `http://<esp32-ip>/capture`.
2. Enter a label such as `living_room_up`, `living_room_stop`, or `living_room_down`.
3. Leave frequency at `433.92` unless you are testing nearby frequencies.
4. Click `Start listening`.
5. Press exactly one physical remote button once.
6. Open `/capture.raw` or `/capture.json` and save the result.

While capture is armed, Home Assistant RF commands are blocked and the RGB LED blinks yellow.

### Option 2: Serial Capture Firmware

Flash the capture firmware:

```powershell
python -m platformio run -e capture -t upload
```

Open the serial monitor:

```powershell
python -m platformio device monitor -e capture
```

Press one remote button at a time. The output looks like:

```text
RAW freq=433.92 count=132 overflow=false rssi=-41 lqi=128 data=[4960, -670, 263, -582, ...]
```

Useful serial commands:

| Command | Action |
| --- | --- |
| `?` | Show help |
| `f` | Try the next likely A-OK frequency |
| `b` | Try the previous likely A-OK frequency |
| `r` | Reset capture at the current frequency |
| `433.92` | Tune directly to a frequency in MHz |

## Decode, Calibrate, And Generate Blind Commands

Decode a saved capture:

```powershell
python .\tools\decode_capture.py .\captures\living_room_open.txt
```

After decoding open, close, and stop for one known channel, build a remote calibration:

```powershell
python .\tools\make_calibration.py `
  --channels 1 `
  --open 0xA1B2C342FEFFF469 `
  --close 0xA1B2C342FEFFBC31 `
  --stop 0xA1B2C342FEFFDC51 `
  --out .\data\remote_calibration.json
```

Create a private blind map:

```powershell
Copy-Item .\data\blinds.example.json .\data\blinds.json
```

Example:

```json
[
  {
    "id": "living_room_curtain",
    "name": "Living Room Curtain",
    "channels": [1],
    "full_travel_ms": 30000,
    "initial_position": -1,
    "invert_position": false
  },
  {
    "id": "guest_blackout",
    "name": "Guest Bedroom Blackout",
    "channels": [2],
    "full_travel_ms": 30000,
    "initial_position": -1,
    "invert_position": false
  },
  {
    "id": "all_blinds",
    "name": "All AOK Blinds",
    "channels": [1, 2],
    "full_travel_ms": 30000,
    "initial_position": -1,
    "invert_position": false
  }
]
```

Generate firmware definitions:

```powershell
python .\tools\generate_blinds_local.py `
  --remote .\data\remote_calibration.json `
  --blinds .\data\blinds.json `
  --out .\include\blinds.local.h
```

For multiple remotes, add a `remote` field to individual blind entries:

```json
{
  "id": "remote2_blind_1",
  "name": "Remote 2 Blind 1",
  "remote": "remote2_calibration.json",
  "channels": [1],
  "full_travel_ms": 30000
}
```

Then rebuild and upload the bridge firmware.

## Home Assistant MQTT Surface

The bridge uses MQTT discovery. For each blind, Home Assistant gets a `cover` entity with:

- Open
- Close
- Stop
- Set position

Open, Close, and Stop remain available at all times while the bridge is online. The
motor does not report its physical position, so MQTT discovery marks each cover as
an assumed-state entity. Home Assistant still displays the bridge's timing-based
position estimate, but an incorrect estimate at `0` or `100` cannot disable a
needed command.

Default base topic:

```text
orvibo_esp32_blinds
```

For a blind ID `living_room_curtain`:

| Purpose | Topic | Payload |
| --- | --- | --- |
| Cover command | `orvibo_esp32_blinds/living_room_curtain/set` | `OPEN`, `CLOSE`, `STOP`, `SYNC_OPEN`, `SYNC_CLOSE`, `STATUS`, or `0-100` |
| Position command | `orvibo_esp32_blinds/living_room_curtain/set_position` | `0-100` |
| Cover state | `orvibo_esp32_blinds/living_room_curtain/state` | `open`, `opening`, `closed`, `closing`, `stopped` |
| Position state | `orvibo_esp32_blinds/living_room_curtain/position` | `0-100` |
| JSON attributes | `orvibo_esp32_blinds/living_room_curtain/attributes` | State and diagnostics JSON |

MQTT discovery topic:

```text
homeassistant/cover/orvibo_esp32_blinds/<blind_id>/config
```

Bridge availability topic:

```text
orvibo_esp32_blinds/status
```

Values:

- `online`
- `offline`

## Position Behavior

The firmware estimates position by time:

- `0` means closed.
- `100` means open.
- The estimate never disables the Open, Close, or Stop controls in Home Assistant.
- Full open and full close can be sent even if current position is unknown.
- Intermediate moves require a known position and a configured stop command.
- After reboot, the position starts unknown unless `initial_position` is configured.
- Use `SYNC_OPEN` or `SYNC_CLOSE` if the blind is already physically at an endpoint and you want to set the estimate without transmitting RF.
- If an intermediate auto-stop fails, the position is marked unknown.

## Web Debug Interface

The bridge runs a small HTTP server after WiFi connects. It is independent of MQTT, so it is useful when Home Assistant is not connected.

Open:

```text
http://<esp32-ip>/
```

Endpoints:

| Endpoint | Purpose |
| --- | --- |
| `/` | Human-readable status, LED table, RF capture form, recent logs, and reboot button |
| `/status` | JSON status for WiFi, MQTT, radio, firmware, LED, logs, and blind state |
| `/logs` | Plain text bounded log |
| `/logs.json` | JSON log entries |
| `/capture` | Browser RF capture page |
| `/capture.raw` | Last RF capture as raw pulse text |
| `/capture.json` | Last RF capture as JSON |
| `/reboot` | Reboot confirmation page |
| `POST /update` | Hidden HTTP firmware upload endpoint |

The visible firmware upload form was intentionally removed from the root page. Use the command-line `curl.exe` upload when a remote firmware update is needed.

## Logs

The in-memory log is bounded:

- Newest 40 entries are kept.
- Long messages are truncated.
- Overwritten entries increment `log_dropped`.
- Before NTP sync, log lines show `time_pending`.
- After NTP sync, log lines start with local date/time and still include milliseconds since boot.

Example:

```text
2026-09-17 09:55:19 #11 +1997ms info MQTT connected
```

The JSON log includes:

- `seq`
- `boot_id`
- `time`
- `ms`
- `level`
- `message`
- `ip`
- `wifi`
- `mqtt_connected`
- `mqtt_state`

## Diagnostics Published To Home Assistant

The firmware publishes MQTT discovery for these bridge diagnostics:

- WiFi RSSI
- Uptime
- IP Address
- Free Heap
- Firmware Version
- CC1101 Radio
- LED Status
- Debug URL
- Last Log
- Log Sequence
- Dropped Logs

See `docs/home-assistant-surface.md` for the detailed entity and topic list.

## If RF Replay Does Not Work

If repeated captures decode differently every time or the motor ignores replayed commands, the motor may use rolling code or encryption. The practical fallback is to electrically press the physical remote buttons with the ESP32 using optocouplers or small relays. In that design, the OEM remote still generates valid RF commands, and Home Assistant only triggers button presses.

## ORVIBO Integration Research

The official Home Assistant ORVIBO integration appears to target older ORVIBO S20 WiFi smart sockets, not the Allone Pro blind/curtain use case.

Community integrations that informed the Home Assistant surface:

- <https://github.com/mozzie1121/orvibohomebridge>
- <https://github.com/kjanko/orvibo-homeassistant-curtains>
- <https://github.com/mozzie1121/orvibo-lan-control>
- <https://github.com/maycode0-0/orvibo-lan>
- <https://github.com/phdindota/orvibo-allone-esphome>

Those projects are references only. This firmware does not import their code.

## License Notes

This repository vendors the SmartRC CC1101 driver library under `lib/SmartRC-CC1101-Driver-Lib`; see that directory for its license.

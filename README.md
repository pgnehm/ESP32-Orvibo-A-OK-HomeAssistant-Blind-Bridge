# ESP32 Orvibo Home Assistant Integration

ESP32 + CC1101 firmware for controlling ORVIBO-managed motorized blinds from Home Assistant by replaying captured RF commands.

This project is the local-control fallback for a blind installation that currently works through an ORVIBO Allone Pro controller and the ORVIBO Home app. The observed controller is an Allone Pro multifunction smart host, model `VS20RB-1GO`, powered by `5V / 1A`. The app exposes multiple blind/curtain entities with open, close, and stop-style controls.

The practical path is:

1. Try existing ORVIBO/HomeMate Home Assistant integrations first.
2. If software integration does not work for the Allone Pro blind entities, use this ESP32 bridge.
3. Capture the blind RF commands.
4. Replay those commands from Home Assistant through MQTT-discovered `cover` entities.

## Current Status

- Platform: ESP32 with Arduino framework via PlatformIO.
- Radio: CC1101 transceiver, default frequency `433.92 MHz`.
- Home Assistant integration: MQTT discovery.
- Entity model: one Home Assistant `cover` per configured blind.
- Position model: timing-based estimate.
- RF commands: placeholder pulse arrays until real open/stop/close commands are captured.
- Build command verified: `python -m platformio run`.

## Why This Exists

Home Assistant's official ORVIBO integration appears to target older ORVIBO S20 Wi-Fi smart sockets, not Allone Pro blind or curtain control.

Two community integrations are still worth testing:

- [`mozzie1121/orvibohomebridge`](https://github.com/mozzie1121/orvibohomebridge) - broader ORVIBO/HomeMate integration with cover support.
- [`kjanko/orvibo-homeassistant-curtains`](https://github.com/kjanko/orvibo-homeassistant-curtains) - focused HomeMate Wi-Fi curtain motor integration.

Both integrations model curtains as Home Assistant `cover` entities with open, close, stop, and set-position behavior. The cloud command shape maps open to value `100`, close to value `0`, and stop to order `stop`. This ESP32 firmware mirrors that Home Assistant surface, but transmits captured RF instead of calling the ORVIBO cloud API.

## Hardware

Minimum working hardware:

- ESP32 development board.
- CC1101 transceiver module.
- Female-to-female Dupont jumper wires.

Recommended debugging hardware:

- RTL-SDR Blog V3 or V4 kit for receive-only RF inspection.
- SMA antenna extension cable if the antenna needs better placement.

Do not power the CC1101 from 5V. Use 3.3V.

### Default CC1101 Wiring

| ESP32 | CC1101 |
| --- | --- |
| 3.3V | VCC |
| GND | GND |
| GPIO18 | SCK |
| GPIO19 | MISO |
| GPIO23 | MOSI |
| GPIO5 | CSN |
| GPIO27 | GDO0 |

`GPIO27` is used as the async OOK transmit data pin. Adjust pins in `include/config.local.h` if your wiring differs.

## Project Layout

- `platformio.ini` - PlatformIO build configuration.
- `src/main.cpp` - WiFi, MQTT discovery, cover command handling, timing estimates, and CC1101 replay.
- `include/config.example.h` - network, MQTT, and radio configuration template.
- `include/blinds.example.h` - blind definitions and RF command template.
- `include/blind_types.h` - shared blind/RF command structs.
- `lib/SmartRC-CC1101-Driver-Lib` - local CC1101 Arduino library copy.
- `docs/home-assistant-surface.md` - all exposed Home Assistant entities, MQTT topics, payloads, and attributes.
- `docs/rf-capture-plan.md` - RF capture workflow.
- `docs/integration-source-notes.md` - notes from the ORVIBO/HomeMate integrations.

## Setup

1. Install PlatformIO.
2. Copy `include/config.example.h` to `include/config.local.h`.
3. Fill in WiFi and MQTT settings.
4. Copy `include/blinds.example.h` to `include/blinds.local.h`.
5. Replace placeholder RF commands with captured pulse arrays.
6. Build:

```powershell
python -m platformio run
```

7. Upload:

```powershell
python -m platformio run -t upload
```

8. Open serial monitor:

```powershell
python -m platformio device monitor
```

The `pio` command can also be used if PlatformIO's executable directory is on PATH.

## Home Assistant MQTT Surface

The default base topic is:

```text
orvibo_esp32_blinds
```

For a blind with ID `living_room_blind`, the firmware exposes:

| Purpose | Topic | Payload |
| --- | --- | --- |
| Cover command | `orvibo_esp32_blinds/living_room_blind/set` | `OPEN`, `CLOSE`, `STOP`, `SYNC_OPEN`, `SYNC_CLOSE`, `STATUS`, or `0-100` |
| Position command | `orvibo_esp32_blinds/living_room_blind/set_position` | `0-100` |
| Cover state | `orvibo_esp32_blinds/living_room_blind/state` | `open`, `opening`, `closed`, `closing`, `stopped` |
| Position state | `orvibo_esp32_blinds/living_room_blind/position` | `0-100` |
| JSON attributes | `orvibo_esp32_blinds/living_room_blind/attributes` | JSON object |
| Availability | `orvibo_esp32_blinds/status` | `online`, `offline` |

MQTT discovery is published under:

```text
homeassistant/cover/orvibo_esp32_blinds/<blind_id>/config
```

### Cover Attributes

Each cover publishes these JSON attributes:

- `id`
- `motion`
- `position_known`
- `estimated_position`
- `target_position`
- `full_travel_ms`
- `invert_position`
- `rf_frequency_mhz`
- `rf_open_configured`
- `rf_close_configured`
- `rf_stop_configured`
- `last_command`
- `last_result`
- `last_error`
- `command_count`
- `failed_command_count`
- `last_command_ms`

### Bridge Diagnostics

The firmware also publishes MQTT discovery for diagnostic entities:

- WiFi RSSI sensor.
- Uptime sensor.
- IP Address sensor.
- Free Heap sensor.
- Firmware Version sensor.
- CC1101 Radio ready/missing binary sensor.

See `docs/home-assistant-surface.md` for the full exposed surface.

## RF Capture Plan

The firmware expects each RF command as a raw pulse train that starts HIGH and alternates HIGH/LOW:

```cpp
static const uint16_t living_open_pulses[] = {
  320, 960, 320, 960
};
```

Capture workflow:

1. Use RTL-SDR to confirm the actual transmit frequency. Start around `433.92 MHz`, but do not assume it is correct.
2. Capture open, stop, and close at least five times for one blind.
3. Compare repeated captures of the same command.
4. If repeated captures are identical, convert each command to pulse arrays for `include/blinds.local.h`.
5. If repeated captures change every time, treat the motor as rolling-code or encrypted.

## Position Behavior

The bridge does not receive real position feedback from the motor. It estimates position from configured full-travel time.

Important behavior:

- Full open and full close can be sent even when position is unknown.
- Intermediate positions require a known current position.
- Intermediate positions require a configured stop RF command.
- After reboot, send `SYNC_OPEN` or `SYNC_CLOSE` if the blind is already at a known endpoint and you do not want to transmit RF.
- If auto-stop fails during an intermediate move, the bridge marks position unknown instead of pretending it reached the target.

## Fallback if RF Replay Fails

If the blinds use rolling-code or encrypted RF, simple replay may not work reliably. The practical DIY fallback is to wire the ESP32 to button contacts on an OEM remote using optocouplers or small relays. The OEM remote continues generating valid commands, while Home Assistant triggers button presses.

## Source References

- [Home Assistant ORVIBO integration](https://www.home-assistant.io/integrations/orvibo)
- [Home Assistant ORVIBO source code](https://github.com/home-assistant/core/tree/dev/homeassistant/components/orvibo)
- [mozzie1121/orvibohomebridge](https://github.com/mozzie1121/orvibohomebridge)
- [kjanko/orvibo-homeassistant-curtains](https://github.com/kjanko/orvibo-homeassistant-curtains)
- [mozzie1121/orvibo-lan-control](https://github.com/mozzie1121/orvibo-lan-control)
- [maycode0-0/orvibo-lan](https://github.com/maycode0-0/orvibo-lan)
- [phdindota/orvibo-allone-esphome](https://github.com/phdindota/orvibo-allone-esphome)
- [Home Assistant Community: Integrating the Orvibo Allone Pro Smart WiFi / RF Hub](https://community.home-assistant.io/t/integrating-the-orvibo-allone-pro-smart-wifi-rf-hub/212675)
- [SmartRC CC1101 Driver Library](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib)

## Notes

This README was created from the Notion project notes titled `Home Assistant Orvibo Blinds Integration Notes - 2026-09-01`, plus the current firmware state in this repository.


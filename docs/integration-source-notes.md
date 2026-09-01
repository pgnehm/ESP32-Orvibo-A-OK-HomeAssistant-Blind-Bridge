# Orvibo integration source notes

The source repos below were inspected as protocol and behavior references only. The ESP32 firmware does not import their code.

Local working copies may exist under `references/orvibo-integrations`, but that folder is intentionally ignored by git to avoid vendoring third-party integrations into this firmware repository.

## orvibohomebridge

Repository: <https://github.com/mozzie1121/orvibohomebridge>

Inspected commit: `a78d3f74ab1d9a8156a45be5504dec5662a87bfb`

Observed behavior:

- Home Assistant platform includes `cover`.
- Supported cover features are open, close, stop, and set position.
- Normal curtains use position commands: `100` opens and `0` closes.
- Stop maps to a cover control call with order `stop`.
- Device classes include curtain and shutter/roller shade depending on device type.
- It supports more than curtains, including lights, sensors, locks, fans, climate, and clothes horse motors.

Relevant upstream files:

- `custom_components/orvibohomebridge/cover.py`
- `custom_components/orvibohomebridge/control.py`
- `custom_components/orvibohomebridge/control_executor.py`
- `custom_components/orvibohomebridge/ssl_client.py`

## orvibo-homeassistant-curtains

Repository: <https://github.com/kjanko/orvibo-homeassistant-curtains>

Inspected commit: `0783e6f12627693bca8ee22ed466134eae6df197`

Observed behavior:

- Home Assistant platform is cover only.
- Setup needs HomeMate/Cuco email, password, and curtain hardware UID.
- It uses REST discovery first, then TLS mutual-auth plus AES-ECB JSON commands on the Orvibo binary API.
- `open_curtain()` sends order `open`, value `100`.
- `close_curtain()` sends order `open`, value `0`.
- `stop_curtain()` sends order `stop`, value `0`.
- It polls position and caches the last known position because the curtain may not always report while idle.

Relevant upstream files:

- `custom_components/orvibo_curtain/api.py`
- `custom_components/orvibo_curtain/cover.py`
- `custom_components/orvibo_curtain/coordinator.py`

## Firmware implication

The ESP32 bridge should expose the same HA cover surface:

- `OPEN`
- `CLOSE`
- `STOP`
- numeric position `0-100`

Because the ESP32 solution will be RF replay, not Orvibo cloud control, real position reporting is unavailable unless we add a separate sensor. The project therefore uses timing-based position tracking and publishes estimated position through MQTT.

# RF capture plan

Goal: learn the AOK remote identity and channel command pattern, then generate clean ESP32 replay commands for every blind and group.

## Hardware

- ESP32 development board.
- CC1101 transceiver module, powered at 3.3V.
- Female-to-female Dupont jumper wires.

Optional:

- RTL-SDR Blog V3 or V4 kit for receive-only inspection. This is helpful, but not required.

## Steps

1. Wire CC1101 GDO0 to the configured ESP32 RF data pin. The tested ESP32-S3 N16R8 board uses GPIO14.
2. Flash capture firmware:

```powershell
python -m platformio run -e capture -t upload
```

3. Open the serial monitor:

```powershell
python -m platformio device monitor -e capture
```

4. Press normal Open, Close, and Stop buttons for one known channel. Do not long-press Program/Pair.
5. If a `RAW ... data=[...]` line appears, paste it into a separate file under `captures/`.
6. If no RAW line appears, type `f` in the serial monitor, press Enter, and press the remote again. The capture firmware steps through nearby 433 MHz frequencies. You can also type a direct frequency like `433.92`.
7. Decode a capture:

```powershell
python .\tools\decode_capture.py .\captures\living_room_ch1_open.txt
```

8. After one channel has Open, Close, and Stop decoded, build calibration:

```powershell
python .\tools\make_calibration.py --channels 1 --open 0x... --close 0x... --stop 0x... --out .\data\remote_calibration.json
```

9. Edit `data/blinds.json` with real blind names and channel numbers.
10. Generate final blind definitions:

```powershell
python .\tools\generate_blinds_local.py --remote .\data\remote_calibration.json --blinds .\data\blinds.json --out .\include\blinds.local.h
```

11. Flash the bridge firmware:

```powershell
python -m platformio run -e esp32dev -t upload
```

If repeated captures change every time and cannot be decoded into stable AOK fields, treat the motor as rolling-code or encrypted. If replay fails because of rolling code, fall back to electrically pressing buttons on an OEM remote with the ESP32.

## Capture firmware serial commands

| Command | Action |
| --- | --- |
| `?` | Show help. |
| `f` | Tune to the next likely AOK 433 MHz frequency. |
| `b` | Tune to the previous likely AOK 433 MHz frequency. |
| `r` | Reset capture at the current frequency. |
| `433.92` | Tune directly to a frequency in MHz. |

The default is `433.92 MHz`, which is the expected AOK remote frequency. The nearby frequency steps are only a fallback if nothing is captured.

## Pulse format expected by firmware

The firmware expects a pulse train array that starts HIGH and alternates HIGH/LOW:

```cpp
static const uint16_t living_open_pulses[] = {
  320, 960, 320, 960
};
```

Each value is a duration in microseconds.

## Initial CC1101 wiring

| CC1101 pin | CC1101 label | ESP32-S3 GPIO |
| --- | --- | --- |
| 1 | GND | GND |
| 2 | VCC | 3.3V |
| 3 | GDO0 | GPIO14 |
| 4 | CSN | GPIO10 |
| 5 | SCK | GPIO12 |
| 6 | MOSI | GPIO11 |
| 7 | MISO/GDO1 | GPIO13 |
| 8 | GDO2 | Not connected |

Do not power the CC1101 from 5V.

## Multi-blind capture notes

The first successful Open/Close/Stop capture gives us the remote identity and command bases. From there the generator can create commands for any channel list in `data/blinds.json`, including individual blinds like `[1]` and groups like `[1, 2, 3]`.

Still capture at least one command from each remote/channel if possible. Those extra captures let us confirm that channel mapping and direction labels match the real installation before transmitting from Home Assistant.

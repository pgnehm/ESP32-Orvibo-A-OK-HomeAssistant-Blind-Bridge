# RF capture plan

Goal: determine whether the blind commands are fixed-code RF and capture clean command pulse trains for ESP32 replay.

## Hardware

- RTL-SDR Blog V3 or V4 kit for receive-only inspection.
- ESP32 development board.
- CC1101 transceiver module, powered at 3.3V.
- Female-to-female Dupont jumper wires.

## Steps

1. Use the RTL-SDR to confirm the actual transmit frequency. Start around 433.92 MHz, but do not assume that is correct.
2. Capture open, stop, and close at least five times for one blind.
3. Compare repeated captures of the same command.
4. If repeated captures are identical, convert each command to pulse arrays for `include/blinds.local.h`.
5. If repeated captures change every time, treat the motor as rolling-code or encrypted.
6. If replay fails because of rolling code, fall back to electrically pressing buttons on an OEM remote with the ESP32.

## Pulse format expected by firmware

The firmware expects a pulse train array that starts HIGH and alternates HIGH/LOW:

```cpp
static const uint16_t living_open_pulses[] = {
  320, 960, 320, 960
};
```

Each value is a duration in microseconds.

## Initial CC1101 wiring

| ESP32 | CC1101 |
| --- | --- |
| 3.3V | VCC |
| GND | GND |
| GPIO18 | SCK |
| GPIO19 | MISO |
| GPIO23 | MOSI |
| GPIO5 | CSN |
| GPIO27 | GDO0 |

Do not power the CC1101 from 5V.


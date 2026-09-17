# AOK remote capture log

Use normal Up, Down, and Stop buttons only. Do not long-press Program/Pair during first capture.

Suggested capture file names:

- `living_room_ch1_open.txt`
- `living_room_ch1_close.txt`
- `living_room_ch1_stop.txt`
- `guest_blackout_ch2_open.txt`
- `all_open.txt`

Paste the whole serial line from the capture firmware:

```text
RAW count=132 overflow=false rssi=-41 lqi=128 data=[4960, -670, 263, -582, ...]
```

Capture each button 3 times if possible. Repeated captures help separate real protocol data from RF noise.

## Worksheet

| Blind | Remote | Channel | Open captured | Close captured | Stop captured | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Living Room Curtain | Main remote | 1 | no | no | no |  |
| Guest Bedroom Blackout | Main remote | 2 | no | no | no |  |
| Guest Bedroom Roller Blind | Main remote | 3 | no | no | no |  |
| All blinds | Main remote | all | no | no | no |  |

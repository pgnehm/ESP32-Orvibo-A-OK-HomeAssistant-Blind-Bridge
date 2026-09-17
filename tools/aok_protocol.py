from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


AOK_ACTION_HIGH = {
    0xF4: "open",
    0xBC: "close",
    0xDC: "stop",
}

OLD_AOK_COMMANDS = {
    11: "open",
    67: "close",
    35: "stop",
    83: "program",
    36: "after_open_close",
}


@dataclass(frozen=True)
class Timings:
    short: int = 263
    long: int = 582
    sync_high: int = 4960
    sync_low: int = 670
    gap: int = 9790
    repeats: int = 8


def parse_int(value: str | int) -> int:
    if isinstance(value, int):
        return value
    value = value.strip()
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def parse_pulses(text: str) -> list[int]:
    data_match = re.search(r"data\s*=\s*\[([^\]]+)\]", text, flags=re.IGNORECASE | re.DOTALL)
    if data_match:
        text = data_match.group(1)
    elif "[" in text and "]" in text:
        bracketed = re.findall(r"\[([^\]]+)\]", text, flags=re.DOTALL)
        if bracketed:
            text = max(bracketed, key=len)
    return [int(match.group(0)) for match in re.finditer(r"-?\d+", text)]


def load_pulses(path: str | Path) -> list[int]:
    return parse_pulses(Path(path).read_text(encoding="utf-8"))


def signed8(value: int) -> int:
    return ((value + 128) % 256) - 128


def channel_field(channels: Iterable[int]) -> int:
    field = 0xFFFF
    for channel in channels:
        channel = int(channel)
        if not 1 <= channel <= 16:
            raise ValueError(f"channel out of range: {channel}")
        field ^= 1 << ((channel + 7) % 16)
    return field


def channels_from_field(field: int) -> list[int]:
    return [channel for channel in range(1, 17) if (field & (1 << ((channel + 7) % 16))) == 0]


def channel_offset(channels: Iterable[int]) -> int:
    return signed8(2 + sum(1 << ((int(channel) - 1) % 8) for channel in channels))


def derive_base(remote_id: int, channels: Iterable[int], captured_command: int) -> int:
    offset = channel_offset(channels)
    return (captured_command & 0xFF00) | ((captured_command - remote_id + offset) & 0xFF)


def make_command(remote_id: int, channels: Iterable[int], base: int) -> int:
    offset = channel_offset(channels)
    return (base & 0xFF00) | ((base + remote_id - offset) & 0xFF)


def make_payload(prefix: int, remote_id: int, channels: Iterable[int], base: int) -> int:
    field = channel_field(channels)
    command = make_command(remote_id, channels, base)
    return (prefix << 40) | (remote_id << 32) | (field << 16) | command


def bits_from_int(value: int, width: int) -> list[int]:
    return [(value >> bit) & 1 for bit in range(width - 1, -1, -1)]


def int_from_bits(bits: Iterable[int]) -> int:
    value = 0
    for bit in bits:
        value = (value << 1) | int(bit)
    return value


def encode_new_pwm_frame(payload: int, timings: Timings = Timings()) -> list[int]:
    raw: list[int] = [timings.sync_high, -timings.sync_low]
    for bit in bits_from_int(payload, 64) + [1, 0]:
        if bit == 0:
            raw.extend([timings.long, -timings.short])
        else:
            raw.extend([timings.short, -timings.long])
    raw.append(-timings.gap)
    return raw


def split_payload(payload: int) -> dict[str, object]:
    prefix = (payload >> 40) & 0xFFFFFF
    remote_id = (payload >> 32) & 0xFF
    field = (payload >> 16) & 0xFFFF
    command = payload & 0xFFFF
    action = AOK_ACTION_HIGH.get((command >> 8) & 0xFF, "unknown")
    return {
        "payload": f"0x{payload:016X}",
        "prefix": f"0x{prefix:06X}",
        "remote_id": f"0x{remote_id:02X}",
        "channel_field": f"0x{field:04X}",
        "channels": channels_from_field(field),
        "command": f"0x{command:04X}",
        "action_guess": action,
    }


def decode_old_aok(payload: int) -> dict[str, object] | None:
    data = payload.to_bytes(8, "big")
    if data[0] != 0xA3:
        return None
    checksum = sum(data[1:7]) & 0xFF
    if checksum != data[7]:
        return None
    address = int.from_bytes(data[4:6], "big")
    command = data[6]
    return {
        "protocol": "old_aok_ac114_candidate",
        "payload": f"0x{payload:016X}",
        "remote_id": f"0x{int.from_bytes(data[1:4], 'big'):06X}",
        "address": f"0x{address:04X}",
        "command": command,
        "action_guess": OLD_AOK_COMMANDS.get(command, "unknown"),
        "checksum": f"0x{data[7]:02X}",
    }


def _classify_pair(first: int, second: int, mapping: str) -> int | None:
    first = abs(first)
    second = abs(second)
    short_min, short_max = 140, 420
    long_min, long_max = 430, 900
    first_short = short_min <= first <= short_max
    first_long = long_min <= first <= long_max
    second_short = short_min <= second <= short_max
    second_long = long_min <= second <= long_max

    if mapping == "new_pwm":
        if first_long and second_short:
            return 0
        if first_short and second_long:
            return 1
    elif mapping == "old_ac114":
        if first_short and second_long:
            return 0
        if first_long and second_short:
            return 1
    else:
        raise ValueError(f"unknown mapping: {mapping}")
    return None


def decode_raw_candidates(pulses: list[int], bits: int = 64) -> list[dict[str, object]]:
    cleaned = [pulse for pulse in pulses if abs(pulse) >= 120]
    candidates: list[dict[str, object]] = []
    seen: set[tuple[str, int, int]] = set()

    for start in range(0, max(0, len(cleaned) - (bits * 2)) + 1):
        for mapping in ("new_pwm", "old_ac114"):
            decoded_bits: list[int] = []
            ok = True
            for idx in range(bits):
                bit = _classify_pair(cleaned[start + (idx * 2)], cleaned[start + (idx * 2) + 1], mapping)
                if bit is None:
                    ok = False
                    break
                decoded_bits.append(bit)
            if not ok:
                continue
            payload = int_from_bits(decoded_bits)
            key = (mapping, payload, start)
            if key in seen:
                continue
            seen.add(key)

            if mapping == "new_pwm":
                parsed = split_payload(payload)
                channels = parsed["channels"]
                action = parsed["action_guess"]
                if channels and (action != "unknown" or len(channels) <= 6):
                    candidates.append(
                        {
                            "protocol": "aok_zemismart_pwm_candidate",
                            "mapping": mapping,
                            "start_pulse_index": start,
                            **parsed,
                        }
                    )
            else:
                old = decode_old_aok(payload)
                if old:
                    old["mapping"] = mapping
                    old["start_pulse_index"] = start
                    candidates.append(old)

    def score(candidate: dict[str, object]) -> tuple[int, int, int, int]:
        action = candidate.get("action_guess")
        protocol = candidate.get("protocol")
        channels = candidate.get("channels")
        channel_count = len(channels) if isinstance(channels, list) else 99
        action_penalty = 0 if action in {"open", "close", "stop"} else 1
        protocol_penalty = 0 if protocol == "aok_zemismart_pwm_candidate" else 1
        return (action_penalty, protocol_penalty, channel_count, int(candidate.get("start_pulse_index", 999999)))

    return sorted(candidates, key=score)


def load_remote_config(path: str | Path) -> dict[str, object]:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def timings_from_config(config: dict[str, object]) -> Timings:
    timings = config.get("timings", {})
    if not isinstance(timings, dict):
        timings = {}
    return Timings(
        short=int(timings.get("short", 263)),
        long=int(timings.get("long", 582)),
        sync_high=int(timings.get("sync_high", 4960)),
        sync_low=int(timings.get("sync_low", 670)),
        gap=int(timings.get("gap", 9790)),
        repeats=int(timings.get("repeats", 8)),
    )


def raw_list_for_action(config: dict[str, object], channels: Iterable[int], action: str) -> list[int]:
    if config.get("protocol") != "aok_zemismart_pwm":
        raise ValueError("only aok_zemismart_pwm generation is implemented")
    bases = config["bases"]
    if not isinstance(bases, dict) or action not in bases:
        raise ValueError(f"missing base for action: {action}")
    prefix = parse_int(config["prefix"])  # type: ignore[index]
    remote_id = parse_int(config["remote_id"])  # type: ignore[index]
    base = parse_int(bases[action])
    timings = timings_from_config(config)
    payload = make_payload(prefix, remote_id, list(channels), base)
    return encode_new_pwm_frame(payload, timings)

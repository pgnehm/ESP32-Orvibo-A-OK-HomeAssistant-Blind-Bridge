from __future__ import annotations

import argparse
import json
from pathlib import Path

from aok_protocol import derive_base, parse_int, split_payload


def parse_channels(value: str) -> list[int]:
    return [int(part.strip()) for part in value.split(",") if part.strip()]


def main() -> int:
    parser = argparse.ArgumentParser(description="Build remote_calibration.json from labeled AOK payloads.")
    parser.add_argument("--channels", required=True, help="Channel list used for the captures, such as 1 or 1,2")
    parser.add_argument("--open", required=True, help="Decoded 64-bit payload for Open, such as 0xA1B2C342FEFFF469")
    parser.add_argument("--close", required=True, help="Decoded 64-bit payload for Close")
    parser.add_argument("--stop", required=True, help="Decoded 64-bit payload for Stop")
    parser.add_argument("--out", type=Path, default=Path("data/remote_calibration.json"))
    args = parser.parse_args()

    channels = parse_channels(args.channels)
    payloads = {
        "open": parse_int(getattr(args, "open")),
        "close": parse_int(args.close),
        "stop": parse_int(args.stop),
    }
    parts = {action: split_payload(payload) for action, payload in payloads.items()}

    prefix = parts["open"]["prefix"]
    remote_id = parts["open"]["remote_id"]
    for action, parsed in parts.items():
        if parsed["prefix"] != prefix or parsed["remote_id"] != remote_id:
            raise SystemExit(f"{action} capture uses a different remote identity")
        if parsed["channels"] != channels:
            raise SystemExit(f"{action} capture decoded channels {parsed['channels']}, expected {channels}")

    remote_id_int = parse_int(str(remote_id))
    bases = {}
    for action, payload in payloads.items():
        command = payload & 0xFFFF
        bases[action] = f"0x{derive_base(remote_id_int, channels, command):04X}"

    config = {
        "protocol": "aok_zemismart_pwm",
        "prefix": prefix,
        "remote_id": remote_id,
        "bases": bases,
        "timings": {
            "short": 263,
            "long": 582,
            "sync_high": 4960,
            "sync_low": 670,
            "gap": 9790,
            "repeats": 8,
        },
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.out}")
    print(json.dumps(config, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

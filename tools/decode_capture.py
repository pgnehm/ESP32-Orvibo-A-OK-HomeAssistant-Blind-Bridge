from __future__ import annotations

import argparse
import json
from pathlib import Path

from aok_protocol import decode_raw_candidates, load_pulses


def main() -> int:
    parser = argparse.ArgumentParser(description="Decode a serial RF capture from an AOK remote.")
    parser.add_argument("capture", type=Path, help="Text file containing RAW pulse data")
    parser.add_argument("--limit", type=int, default=12, help="Maximum candidates to print")
    args = parser.parse_args()

    pulses = load_pulses(args.capture)
    print(f"Loaded {len(pulses)} pulse durations from {args.capture}")

    candidates = decode_raw_candidates(pulses)
    if not candidates:
        print("No AOK candidates found.")
        print("Try another capture with the remote closer to the CC1101 antenna, or capture with RTL-SDR.")
        return 2

    for idx, candidate in enumerate(candidates[: args.limit], 1):
        print(f"\nCandidate {idx}")
        print(json.dumps(candidate, indent=2, sort_keys=False))
    if len(candidates) > args.limit:
        print(f"\nHidden lower-confidence candidates: {len(candidates) - args.limit}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

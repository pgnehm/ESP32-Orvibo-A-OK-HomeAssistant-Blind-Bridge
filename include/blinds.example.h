#pragma once

#include "blind_types.h"

// Raw pulse trains start HIGH and alternate HIGH/LOW for each duration.
// Replace nullptr/0 entries with captured pulse arrays once RF learning is done.
//
// Example shape:
// static const uint16_t living_open_pulses[] = { 320, 960, 320, 960 };
// RawRfCommand{"open", living_open_pulses, sizeof(living_open_pulses) / sizeof(uint16_t), 5, 35}

static const BlindDefinition BLINDS[] = {
  {
    "living_room_blind",
    "Living Room Blind",
    30000,
    -1,
    false,
    RawRfCommand{"open", nullptr, 0, 5, 35},
    RawRfCommand{"close", nullptr, 0, 5, 35},
    RawRfCommand{"stop", nullptr, 0, 5, 35},
  },
};

static constexpr size_t BLIND_COUNT = sizeof(BLINDS) / sizeof(BLINDS[0]);


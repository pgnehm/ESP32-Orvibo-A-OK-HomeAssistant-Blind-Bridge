#pragma once

#include <Arduino.h>

struct RawRfCommand {
  const char* label;
  const uint16_t* pulses;
  size_t pulseCount;
  uint8_t repeats;
  uint16_t interFrameGapMs;
};

struct BlindDefinition {
  const char* id;
  const char* name;
  uint32_t fullTravelMs;
  int initialPosition;
  bool invertPosition;
  RawRfCommand openCommand;
  RawRfCommand closeCommand;
  RawRfCommand stopCommand;
};


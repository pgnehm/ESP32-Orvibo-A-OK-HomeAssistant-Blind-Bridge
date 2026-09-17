#ifdef RF_CAPTURE_MODE

#include <Arduino.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#include "config.h"

#ifndef STATUS_RGB_PIN
#if defined(RGB_BUILTIN)
#define STATUS_RGB_PIN RGB_BUILTIN
#else
#define STATUS_RGB_PIN -1
#endif
#endif

namespace {

constexpr size_t MAX_PULSES = 700;
constexpr uint32_t MIN_PULSE_US = 90;
constexpr uint32_t MAX_PULSE_US = 65000;
constexpr uint32_t IDLE_FRAME_US = 18000;

constexpr float FREQUENCY_CANDIDATES_MHZ[] = {
  433.42f,
  433.62f,
  433.82f,
  433.92f,
  434.02f,
  434.22f,
  434.42f,
};

volatile int32_t pulses[MAX_PULSES];
volatile size_t pulseCount = 0;
volatile uint32_t lastEdgeUs = 0;
volatile uint32_t lastFrameEdgeUs = 0;
volatile bool captureActive = false;
volatile bool overflowed = false;

bool radioReady = false;
float currentFrequencyMhz = RF_FREQUENCY_MHZ;
size_t currentFrequencyIndex = 3;
uint32_t lastReadyPrintMs = 0;
uint32_t lastStatusLedMs = 0;
bool statusLedOn = false;

void IRAM_ATTR onRfEdge();

void resetCaptureState() {
  noInterrupts();
  pulseCount = 0;
  captureActive = false;
  overflowed = false;
  lastEdgeUs = 0;
  lastFrameEdgeUs = 0;
  interrupts();
}

void setStatusLed(uint8_t red, uint8_t green, uint8_t blue) {
#if STATUS_RGB_PIN >= 0
  neopixelWrite(STATUS_RGB_PIN, red, green, blue);
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, red > 0 || green > 0 || blue > 0 ? HIGH : LOW);
#else
  (void)red;
  (void)green;
  (void)blue;
#endif
}

void updateStatusLed(uint32_t nowMs) {
  if (nowMs - lastStatusLedMs < 1000) {
    return;
  }
  lastStatusLedMs = nowMs;
  statusLedOn = !statusLedOn;
  if (!statusLedOn) {
    setStatusLed(0, 0, 0);
  } else if (radioReady) {
    setStatusLed(0, 16, 0);
  } else {
    setStatusLed(16, 0, 0);
  }
}

void tuneRadio(float frequencyMhz) {
  currentFrequencyMhz = frequencyMhz;
  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  resetCaptureState();
  ELECHOUSE_cc1101.SetRx(currentFrequencyMhz);
  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), onRfEdge, CHANGE);
  Serial.printf("Tuned CC1101 to %.2f MHz. Press a remote button now.\n", currentFrequencyMhz);
}

void tuneCandidate(size_t index) {
  currentFrequencyIndex = index % (sizeof(FREQUENCY_CANDIDATES_MHZ) / sizeof(FREQUENCY_CANDIDATES_MHZ[0]));
  tuneRadio(FREQUENCY_CANDIDATES_MHZ[currentFrequencyIndex]);
}

void printHelp() {
  Serial.println();
  Serial.println("Serial commands:");
  Serial.println("  ?        show this help");
  Serial.println("  f        next likely AOK frequency");
  Serial.println("  b        previous likely AOK frequency");
  Serial.println("  r        retune/reset capture at current frequency");
  Serial.println("  433.92   tune directly to a frequency in MHz");
  Serial.println();
}

void IRAM_ATTR onRfEdge() {
  const uint32_t now = micros();
  const bool currentLevel = digitalRead(RF_RX_PIN);

  if (!captureActive) {
    captureActive = true;
    pulseCount = 0;
    overflowed = false;
    lastEdgeUs = now;
    lastFrameEdgeUs = now;
    return;
  }

  const uint32_t duration = now - lastEdgeUs;
  lastEdgeUs = now;
  lastFrameEdgeUs = now;

  if (duration < MIN_PULSE_US || duration > MAX_PULSE_US) {
    return;
  }

  const bool previousLevel = !currentLevel;
  const int32_t signedDuration = previousLevel ? static_cast<int32_t>(duration) : -static_cast<int32_t>(duration);

  if (pulseCount < MAX_PULSES) {
    pulses[pulseCount++] = signedDuration;
  } else {
    overflowed = true;
  }
}

void setupRadio() {
  pinMode(RF_RX_PIN, INPUT);

  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CS_PIN);
  ELECHOUSE_cc1101.setGDO0(RF_RX_PIN);
  ELECHOUSE_cc1101.Init();
  radioReady = ELECHOUSE_cc1101.getCC1101();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(RF_FREQUENCY_MHZ);
  ELECHOUSE_cc1101.setPA(RF_PA_DBM);
  ELECHOUSE_cc1101.setRxBW(270.0);
  ELECHOUSE_cc1101.setDRate(4.8);
  ELECHOUSE_cc1101.setSyncMode(0);
  ELECHOUSE_cc1101.setCrc(false);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.SetRx(RF_FREQUENCY_MHZ);

  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), onRfEdge, CHANGE);
}

void printCapture() {
  int32_t localPulses[MAX_PULSES];
  size_t localCount = 0;
  bool localOverflow = false;

  noInterrupts();
  localCount = pulseCount;
  if (localCount > MAX_PULSES) {
    localCount = MAX_PULSES;
  }
  for (size_t index = 0; index < localCount; ++index) {
    localPulses[index] = pulses[index];
  }
  localOverflow = overflowed;
  pulseCount = 0;
  captureActive = false;
  overflowed = false;
  interrupts();

  if (localCount < 20) {
    return;
  }

  Serial.printf("RAW freq=%.2f count=%u overflow=%s rssi=%d lqi=%u data=[",
                currentFrequencyMhz,
                static_cast<unsigned>(localCount),
                localOverflow ? "true" : "false",
                ELECHOUSE_cc1101.getRssi(),
                static_cast<unsigned>(ELECHOUSE_cc1101.getLqi()));
  for (size_t index = 0; index < localCount; ++index) {
    if (index > 0) {
      Serial.print(", ");
    }
    Serial.print(localPulses[index]);
  }
  Serial.println("]");
}

}  // namespace

void setup() {
#if defined(LED_BUILTIN) && !defined(RGB_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  setStatusLed(0, 0, 16);

  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  Serial.println("AOK RF capture firmware");
  Serial.printf("Frequency: %.2f MHz\n", RF_FREQUENCY_MHZ);
  Serial.printf("RX pin: GPIO%d\n", RF_RX_PIN);

  setupRadio();
  Serial.printf("CC1101 %s\n", radioReady ? "ready" : "not detected");
  Serial.println("Press one remote button at a time. Capture Up, Down, and Stop for each channel.");
  Serial.println("If no RAW line appears, type 'f' in the serial monitor and press the remote again.");
  printHelp();
}

void loop() {
  const uint32_t nowMs = millis();
  updateStatusLed(nowMs);

  if (nowMs - lastReadyPrintMs > 5000) {
    lastReadyPrintMs = nowMs;
    Serial.printf("READY freq=%.2f radio=%s pulses=%u\n",
                  currentFrequencyMhz,
                  radioReady ? "ready" : "not_detected",
                  static_cast<unsigned>(pulseCount));
  }

  const bool frameEnded = captureActive && (micros() - lastFrameEdgeUs > IDLE_FRAME_US);
  if (frameEnded) {
    detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
    printCapture();
    attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), onRfEdge, CHANGE);
  }

  if (Serial.available() > 0) {
    const String command = Serial.readStringUntil('\n');
    String trimmed = command;
    trimmed.trim();
    trimmed.toLowerCase();
    if (trimmed == "?" || trimmed == "h" || trimmed == "help") {
      printHelp();
    } else if (trimmed == "f" || trimmed == "next") {
      tuneCandidate(currentFrequencyIndex + 1);
    } else if (trimmed == "b" || trimmed == "back" || trimmed == "prev") {
      const size_t count = sizeof(FREQUENCY_CANDIDATES_MHZ) / sizeof(FREQUENCY_CANDIDATES_MHZ[0]);
      tuneCandidate((currentFrequencyIndex + count - 1) % count);
    } else if (trimmed == "r" || trimmed == "reset") {
      tuneRadio(currentFrequencyMhz);
    } else if (trimmed.length() > 0) {
      const float requestedFrequency = trimmed.toFloat();
      if (requestedFrequency >= 300.0f && requestedFrequency <= 928.0f) {
        tuneRadio(requestedFrequency);
      } else {
        Serial.printf("Unknown command: %s\n", trimmed.c_str());
        printHelp();
      }
    }
  }
  delay(2);
}

#endif  // RF_CAPTURE_MODE

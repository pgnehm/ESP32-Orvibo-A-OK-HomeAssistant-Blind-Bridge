#ifndef RF_CAPTURE_MODE

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <time.h>

#include "blinds.h"
#include "config.h"

#ifndef STATUS_RGB_PIN
#if defined(RGB_BUILTIN)
#define STATUS_RGB_PIN RGB_BUILTIN
#else
#define STATUS_RGB_PIN -1
#endif
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

#ifndef OTA_PORT
#define OTA_PORT 3232
#endif

#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "pool.ntp.org"
#endif

#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "time.nist.gov"
#endif

#ifndef TIME_ZONE
#define TIME_ZONE "EST5EDT,M3.2.0/2,M11.1.0/2"
#endif

namespace {

constexpr size_t REMOTE_LOG_BUFFER_SIZE = 40;
constexpr size_t REMOTE_LOG_MESSAGE_MAX = 220;
constexpr size_t RF_CAPTURE_MAX_PULSES = 2000;
constexpr uint32_t RF_CAPTURE_MIN_PULSE_US = 90;
constexpr uint32_t RF_CAPTURE_MAX_PULSE_US = 65000;
constexpr uint32_t RF_CAPTURE_IDLE_FRAME_US = 18000;
constexpr uint32_t RF_CAPTURE_DEFAULT_TIMEOUT_MS = 15000;
constexpr uint32_t RF_CAPTURE_MAX_TIMEOUT_MS = 60000;

enum class Motion {
  Stopped,
  Opening,
  Closing,
};

enum class LedMode {
  Booting,
  WifiConnecting,
  MqttConnecting,
  Online,
  RadioMissing,
  RfTransmit,
  RfCapture,
  OtaUpdate,
  Rebooting,
  Error,
};

struct RemoteLogEntry {
  uint32_t sequence = 0;
  uint32_t ms = 0;
  String level;
  String message;
  String ip;
  String wifi;
  bool mqttConnected = false;
  int mqttState = 0;
};

struct BlindRuntime {
  const BlindDefinition* definition = nullptr;
  int position = -1;
  int startPosition = -1;
  int targetPosition = -1;
  uint32_t motionStartedMs = 0;
  uint32_t motionDurationMs = 0;
  uint32_t lastPositionPublishMs = 0;
  Motion motion = Motion::Stopped;
  String lastCommand = "none";
  String lastResult = "idle";
  String lastError = "";
  uint32_t commandCount = 0;
  uint32_t failedCommandCount = 0;
  uint32_t lastCommandMs = 0;
};

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer debugServer(80);
BlindRuntime blindStates[BLIND_COUNT];

bool radioReady = false;
bool debugServerStarted = false;
bool otaStarted = false;
bool otaInProgress = false;
bool rebootPending = false;
bool httpUpdateOk = false;
String httpUpdateMessage;
bool timeSyncStarted = false;
bool timeSyncedLogged = false;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t lastDiagnosticsPublishMs = 0;
uint32_t lastTimeSyncCheckMs = 0;
uint32_t rebootAtMs = 0;
uint32_t bootId = 0;
uint32_t logSequence = 0;
uint32_t lastMqttLogSequence = 0;
uint32_t remoteLogDroppedCount = 0;
RemoteLogEntry remoteLogs[REMOTE_LOG_BUFFER_SIZE];
size_t remoteLogStart = 0;
size_t remoteLogCount = 0;
LedMode ledHoldMode = LedMode::Booting;
uint32_t ledHoldUntilMs = 0;
uint32_t lastLedUpdateMs = 0;
String currentLedStatus = "booting";
wl_status_t lastLoggedWifiStatus = WL_NO_SHIELD;
bool lastLoggedMqttConnected = false;
int lastLoggedMqttState = 999;
volatile int32_t rfCapturePulses[RF_CAPTURE_MAX_PULSES];
volatile size_t rfCapturePulseCount = 0;
volatile uint32_t rfCaptureLastEdgeUs = 0;
volatile uint32_t rfCaptureLastFrameEdgeUs = 0;
volatile bool rfCaptureArmed = false;
volatile bool rfCaptureActive = false;
volatile bool rfCaptureOverflow = false;
int32_t rfCaptureScratchPulses[RF_CAPTURE_MAX_PULSES];
int32_t lastRfCapturePulses[RF_CAPTURE_MAX_PULSES];
size_t lastRfCaptureCount = 0;
bool lastRfCaptureOverflow = false;
uint32_t lastRfCaptureSequence = 0;
uint32_t lastRfCaptureMs = 0;
int lastRfCaptureRssi = 0;
uint8_t lastRfCaptureLqi = 0;
float rfCaptureFrequencyMhz = RF_FREQUENCY_MHZ;
uint32_t rfCaptureStartedMs = 0;
uint32_t rfCaptureTimeoutMs = RF_CAPTURE_DEFAULT_TIMEOUT_MS;
String rfCaptureLabel = "";
String rfCaptureStatus = "idle";
uint32_t rfCaptureIgnoredShortFrames = 0;

void IRAM_ATTR onRfCaptureEdge();

int clampPosition(int value) {
  if (value < 0) {
    return 0;
  }
  if (value > 100) {
    return 100;
  }
  return value;
}

uint32_t maxU32(uint32_t a, uint32_t b) {
  return a > b ? a : b;
}

const char* motionToString(Motion motion) {
  switch (motion) {
    case Motion::Opening:
      return "opening";
    case Motion::Closing:
      return "closing";
    case Motion::Stopped:
    default:
      return "stopped";
  }
}

const char* boolJson(bool value) {
  return value ? "true" : "false";
}

bool isRfConfigured(const RawRfCommand& command) {
  return command.pulses != nullptr && command.pulseCount > 0 && command.repeats > 0;
}

String jsonEscape(const char* value) {
  String output;
  if (value == nullptr) {
    return output;
  }
  for (const char* cursor = value; *cursor != '\0'; ++cursor) {
    if (*cursor == '"' || *cursor == '\\') {
      output += '\\';
    }
    output += *cursor;
  }
  return output;
}

String jsonEscape(const String& value) {
  return jsonEscape(value.c_str());
}

String bridgeAvailabilityTopic() {
  return String(MQTT_BASE_TOPIC) + "/status";
}

String bridgeTopic(const char* suffix) {
  return String(MQTT_BASE_TOPIC) + "/" + suffix;
}

String diagnosticTopic(const char* suffix) {
  return String(MQTT_BASE_TOPIC) + "/diagnostics/" + suffix;
}

String blindTopic(const BlindDefinition& blind, const char* suffix) {
  return String(MQTT_BASE_TOPIC) + "/" + blind.id + "/" + suffix;
}

String discoveryTopic(const BlindDefinition& blind) {
  return String(MQTT_DISCOVERY_PREFIX) + "/cover/" + DEVICE_ID + "/" + blind.id + "/config";
}

String entityDiscoveryTopic(const char* platform, const char* objectId) {
  return String(MQTT_DISCOVERY_PREFIX) + "/" + platform + "/" + DEVICE_ID + "/" + objectId + "/config";
}

String logTopic() {
  return bridgeTopic("log");
}

String bootIdText() {
  String value = String(bootId, HEX);
  value.toUpperCase();
  return value;
}

String wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "no_ssid";
    case WL_SCAN_COMPLETED:
      return "scan_completed";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    case WL_NO_SHIELD:
    default:
      return "not_started";
  }
}

String compactSensorText(const String& value) {
  constexpr size_t maxLength = 240;
  if (value.length() <= maxLength) {
    return value;
  }
  String output = value.substring(0, maxLength - 3);
  output += "...";
  return output;
}

String boundedLogMessage(const String& value) {
  if (value.length() <= REMOTE_LOG_MESSAGE_MAX) {
    return value;
  }
  String output = value.substring(0, REMOTE_LOG_MESSAGE_MAX - 3);
  output += "...";
  return output;
}

bool wallClockReady() {
  return time(nullptr) > 1700000000;
}

String formatEpochLocal(time_t value) {
  struct tm localTime;
  if (!localtime_r(&value, &localTime)) {
    return "";
  }

  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);
  return String(buffer);
}

String logEntryTimeText(uint32_t eventMs) {
  const time_t now = time(nullptr);
  if (now <= 1700000000) {
    return "";
  }

  const uint32_t elapsedSinceEventMs = millis() - eventMs;
  const time_t eventTime = now - static_cast<time_t>(elapsedSinceEventMs / 1000UL);
  return formatEpochLocal(eventTime);
}

void appendRemoteLog(const RemoteLogEntry& entry) {
  if (remoteLogCount < REMOTE_LOG_BUFFER_SIZE) {
    const size_t index = (remoteLogStart + remoteLogCount) % REMOTE_LOG_BUFFER_SIZE;
    remoteLogs[index] = entry;
    remoteLogCount++;
    return;
  }

  remoteLogs[remoteLogStart] = entry;
  remoteLogStart = (remoteLogStart + 1) % REMOTE_LOG_BUFFER_SIZE;
  remoteLogDroppedCount++;
}

String remoteLogJson(const RemoteLogEntry& entry) {
  String payload;
  payload.reserve(320 + entry.message.length());
  payload += "{";
  payload += "\"seq\":";
  payload += String(entry.sequence);
  payload += ",";
  payload += "\"boot_id\":\"";
  payload += bootIdText();
  payload += "\",";
  payload += "\"time\":";
  const String timeText = logEntryTimeText(entry.ms);
  if (timeText.length() > 0) {
    payload += "\"";
    payload += timeText;
    payload += "\"";
  } else {
    payload += "null";
  }
  payload += ",";
  payload += "\"ms\":";
  payload += String(entry.ms);
  payload += ",";
  payload += "\"level\":\"";
  payload += jsonEscape(entry.level);
  payload += "\",";
  payload += "\"message\":\"";
  payload += jsonEscape(entry.message);
  payload += "\",";
  payload += "\"ip\":\"";
  payload += jsonEscape(entry.ip);
  payload += "\",";
  payload += "\"wifi\":\"";
  payload += jsonEscape(entry.wifi);
  payload += "\",";
  payload += "\"mqtt_connected\":";
  payload += (entry.mqttConnected ? "true" : "false");
  payload += ",";
  payload += "\"mqtt_state\":";
  payload += String(entry.mqttState);
  payload += "}";
  return payload;
}

String remoteLogLine(const RemoteLogEntry& entry) {
  String line;
  line.reserve(72 + entry.level.length() + entry.message.length());
  const String timeText = logEntryTimeText(entry.ms);
  line += timeText.length() > 0 ? timeText : "time_pending";
  line += " #";
  line += String(entry.sequence);
  line += " +";
  line += String(entry.ms);
  line += "ms ";
  line += entry.level;
  line += " ";
  line += entry.message;
  return line;
}

void publishRemoteLogToMqtt(const RemoteLogEntry& entry) {
  if (!mqtt.connected()) {
    return;
  }

  const String payload = remoteLogJson(entry);
  const String line = compactSensorText(remoteLogLine(entry));
  const String topic = logTopic();
  const String lastLogTopic = diagnosticTopic("last_log");
  const String lastLogLevelTopic = diagnosticTopic("last_log_level");
  const String lastLogMsTopic = diagnosticTopic("last_log_ms");
  const String sequenceTopic = diagnosticTopic("log_sequence");

  mqtt.publish(topic.c_str(), payload.c_str(), false);
  mqtt.publish(lastLogTopic.c_str(), line.c_str(), MQTT_RETAIN);
  mqtt.publish(lastLogLevelTopic.c_str(), entry.level.c_str(), MQTT_RETAIN);
  mqtt.publish(lastLogMsTopic.c_str(), String(entry.ms).c_str(), MQTT_RETAIN);
  mqtt.publish(sequenceTopic.c_str(), String(entry.sequence).c_str(), MQTT_RETAIN);
  lastMqttLogSequence = entry.sequence;
}

void publishPendingRemoteLogsToMqtt() {
  if (!mqtt.connected()) {
    return;
  }

  for (size_t offset = 0; offset < remoteLogCount; ++offset) {
    const size_t index = (remoteLogStart + offset) % REMOTE_LOG_BUFFER_SIZE;
    const RemoteLogEntry& entry = remoteLogs[index];
    if (entry.sequence > lastMqttLogSequence) {
      publishRemoteLogToMqtt(entry);
    }
  }
}

void remoteLog(const char* level, const String& message) {
  RemoteLogEntry entry;
  entry.sequence = ++logSequence;
  entry.ms = millis();
  entry.level = level;
  entry.message = boundedLogMessage(message);
  entry.ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  entry.wifi = wifiStatusText(WiFi.status());
  entry.mqttConnected = mqtt.connected();
  entry.mqttState = mqtt.state();
  appendRemoteLog(entry);

  Serial.println(remoteLogLine(entry));
  publishRemoteLogToMqtt(entry);
}

void remoteLog(const char* level, const char* message) {
  remoteLog(level, String(message));
}

const char* ledModeName(LedMode mode) {
  switch (mode) {
    case LedMode::Booting:
      return "booting";
    case LedMode::WifiConnecting:
      return "wifi_connecting";
    case LedMode::MqttConnecting:
      return "mqtt_connecting";
    case LedMode::Online:
      return "online";
    case LedMode::RadioMissing:
      return "radio_missing";
    case LedMode::RfTransmit:
      return "rf_transmit";
    case LedMode::RfCapture:
      return "rf_capture";
    case LedMode::OtaUpdate:
      return "ota_update";
    case LedMode::Rebooting:
      return "rebooting";
    case LedMode::Error:
      return "error";
    default:
      return "unknown";
  }
}

const char* otaErrorText(ota_error_t error) {
  switch (error) {
    case OTA_AUTH_ERROR:
      return "auth";
    case OTA_BEGIN_ERROR:
      return "begin";
    case OTA_CONNECT_ERROR:
      return "connect";
    case OTA_RECEIVE_ERROR:
      return "receive";
    case OTA_END_ERROR:
      return "end";
    default:
      return "unknown";
  }
}

void setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue) {
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

void initStatusLed() {
#if defined(LED_BUILTIN) && STATUS_RGB_PIN < 0
  pinMode(LED_BUILTIN, OUTPUT);
#endif
  currentLedStatus = ledModeName(LedMode::Booting);
  setStatusLedColor(0, 0, 24);
}

LedMode currentDesiredLedMode() {
  const uint32_t now = millis();
  if (ledHoldUntilMs > now) {
    return ledHoldMode;
  }
  if (rfCaptureArmed) {
    return LedMode::RfCapture;
  }
  if (!radioReady) {
    return LedMode::RadioMissing;
  }
  if (otaInProgress) {
    return LedMode::OtaUpdate;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return LedMode::WifiConnecting;
  }
  if (!mqtt.connected()) {
    return LedMode::MqttConnecting;
  }
  return LedMode::Online;
}

void holdStatusLed(LedMode mode, uint32_t durationMs) {
  ledHoldMode = mode;
  ledHoldUntilMs = millis() + durationMs;
  currentLedStatus = ledModeName(mode);
  switch (mode) {
    case LedMode::RfTransmit:
      setStatusLedColor(40, 40, 40);
      break;
    case LedMode::Error:
      setStatusLedColor(48, 0, 0);
      break;
    case LedMode::Rebooting:
      setStatusLedColor(30, 0, 30);
      break;
    case LedMode::OtaUpdate:
      setStatusLedColor(0, 28, 32);
      break;
    case LedMode::RfCapture:
      setStatusLedColor(32, 24, 0);
      break;
    default:
      break;
  }
}

void updateStatusLed() {
  const uint32_t now = millis();
  if (now - lastLedUpdateMs < 100) {
    return;
  }
  lastLedUpdateMs = now;

  const LedMode mode = currentDesiredLedMode();
  currentLedStatus = ledModeName(mode);
  const bool slowBlink = ((now / 500U) % 2U) == 0;
  const bool fastBlink = ((now / 250U) % 2U) == 0;

  switch (mode) {
    case LedMode::Booting:
      setStatusLedColor(0, 0, 24);
      break;
    case LedMode::WifiConnecting:
      setStatusLedColor(slowBlink ? 32 : 0, slowBlink ? 14 : 0, 0);
      break;
    case LedMode::MqttConnecting:
      setStatusLedColor(0, 0, slowBlink ? 32 : 0);
      break;
    case LedMode::Online:
      setStatusLedColor(0, 18, 0);
      break;
    case LedMode::RadioMissing:
      setStatusLedColor(fastBlink ? 48 : 0, 0, 0);
      break;
    case LedMode::RfTransmit:
      setStatusLedColor(40, 40, 40);
      break;
    case LedMode::RfCapture:
      setStatusLedColor(fastBlink ? 32 : 0, fastBlink ? 24 : 0, 0);
      break;
    case LedMode::OtaUpdate:
      setStatusLedColor(0, fastBlink ? 28 : 0, fastBlink ? 32 : 0);
      break;
    case LedMode::Rebooting:
      setStatusLedColor(30, 0, 30);
      break;
    case LedMode::Error:
      setStatusLedColor(fastBlink ? 48 : 0, 0, 0);
      break;
  }
}

void IRAM_ATTR onRfCaptureEdge() {
  if (!rfCaptureArmed) {
    return;
  }

  const uint32_t now = micros();
  const bool currentLevel = digitalRead(RF_RX_PIN);

  if (!rfCaptureActive) {
    rfCaptureActive = true;
    rfCapturePulseCount = 0;
    rfCaptureOverflow = false;
    rfCaptureLastEdgeUs = now;
    rfCaptureLastFrameEdgeUs = now;
    return;
  }

  const uint32_t duration = now - rfCaptureLastEdgeUs;
  rfCaptureLastEdgeUs = now;
  rfCaptureLastFrameEdgeUs = now;

  if (duration < RF_CAPTURE_MIN_PULSE_US || duration > RF_CAPTURE_MAX_PULSE_US) {
    return;
  }

  const bool previousLevel = !currentLevel;
  const int32_t signedDuration = previousLevel ? static_cast<int32_t>(duration) : -static_cast<int32_t>(duration);

  if (rfCapturePulseCount < RF_CAPTURE_MAX_PULSES) {
    rfCapturePulses[rfCapturePulseCount++] = signedDuration;
  } else {
    rfCaptureOverflow = true;
  }
}

void resetRfCaptureState() {
  noInterrupts();
  rfCapturePulseCount = 0;
  rfCaptureLastEdgeUs = 0;
  rfCaptureLastFrameEdgeUs = 0;
  rfCaptureActive = false;
  rfCaptureOverflow = false;
  interrupts();
}

void restoreRadioTxMode() {
  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  pinMode(RF_TX_PIN, OUTPUT);
  digitalWrite(RF_TX_PIN, LOW);
  if (radioReady) {
    ELECHOUSE_cc1101.SetTx(RF_FREQUENCY_MHZ);
  }
}

String rfCaptureRawText() {
  String payload;
  if (lastRfCaptureCount == 0) {
    payload = "No RF capture available yet.\n";
    return payload;
  }

  payload.reserve(150 + (lastRfCaptureCount * 8));
  payload += "RAW label=\"";
  payload += rfCaptureLabel;
  payload += "\" seq=";
  payload += String(lastRfCaptureSequence);
  payload += " freq=";
  payload += String(rfCaptureFrequencyMhz, 2);
  payload += " count=";
  payload += String(lastRfCaptureCount);
  payload += " overflow=";
  payload += lastRfCaptureOverflow ? "true" : "false";
  payload += " rssi=";
  payload += String(lastRfCaptureRssi);
  payload += " lqi=";
  payload += String(lastRfCaptureLqi);
  payload += " data=[";
  for (size_t index = 0; index < lastRfCaptureCount; ++index) {
    if (index > 0) {
      payload += ", ";
    }
    payload += String(lastRfCapturePulses[index]);
  }
  payload += "]\n";
  return payload;
}

String rfCaptureJson() {
  String payload;
  payload.reserve(420 + (lastRfCaptureCount * 8));
  payload += "{";
  payload += "\"status\":\"";
  payload += jsonEscape(rfCaptureStatus);
  payload += "\",";
  payload += "\"armed\":";
  payload += rfCaptureArmed ? "true" : "false";
  payload += ",";
  payload += "\"active\":";
  payload += rfCaptureActive ? "true" : "false";
  payload += ",";
  payload += "\"label\":\"";
  payload += jsonEscape(rfCaptureLabel);
  payload += "\",";
  payload += "\"sequence\":";
  payload += String(lastRfCaptureSequence);
  payload += ",";
  payload += "\"frequency_mhz\":";
  payload += String(rfCaptureFrequencyMhz, 2);
  payload += ",";
  payload += "\"count\":";
  payload += String(lastRfCaptureCount);
  payload += ",";
  payload += "\"ignored_short_frames\":";
  payload += String(rfCaptureIgnoredShortFrames);
  payload += ",";
  payload += "\"overflow\":";
  payload += lastRfCaptureOverflow ? "true" : "false";
  payload += ",";
  payload += "\"rssi\":";
  payload += String(lastRfCaptureRssi);
  payload += ",";
  payload += "\"lqi\":";
  payload += String(lastRfCaptureLqi);
  payload += ",";
  payload += "\"age_ms\":";
  payload += lastRfCaptureMs > 0 ? String(millis() - lastRfCaptureMs) : String("null");
  payload += ",";
  payload += "\"data\":[";
  for (size_t index = 0; index < lastRfCaptureCount; ++index) {
    if (index > 0) {
      payload += ",";
    }
    payload += String(lastRfCapturePulses[index]);
  }
  payload += "]}";
  return payload;
}

void beginRfCapture(const String& label, float frequencyMhz, uint32_t timeoutMs) {
  if (!radioReady) {
    rfCaptureStatus = "radio_missing";
    remoteLog("error", "Cannot arm RF capture; CC1101 radio is not ready");
    holdStatusLed(LedMode::Error, 3000);
    return;
  }

  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  rfCaptureLabel = label.length() > 0 ? label : String("unnamed");
  rfCaptureFrequencyMhz = frequencyMhz;
  rfCaptureTimeoutMs = timeoutMs;
  if (rfCaptureTimeoutMs == 0 || rfCaptureTimeoutMs > RF_CAPTURE_MAX_TIMEOUT_MS) {
    rfCaptureTimeoutMs = RF_CAPTURE_DEFAULT_TIMEOUT_MS;
  }

  resetRfCaptureState();
  pinMode(RF_RX_PIN, INPUT);
  ELECHOUSE_cc1101.setGDO0(RF_RX_PIN);
  ELECHOUSE_cc1101.setRxBW(270.0);
  ELECHOUSE_cc1101.setDRate(4.8);
  ELECHOUSE_cc1101.SetRx(rfCaptureFrequencyMhz);

  noInterrupts();
  rfCaptureArmed = true;
  interrupts();
  rfCaptureStartedMs = millis();
  rfCaptureStatus = "armed";
  rfCaptureIgnoredShortFrames = 0;
  attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), onRfCaptureEdge, CHANGE);
  remoteLog("warn",
            String("RF capture armed label=") + rfCaptureLabel +
              " freq=" + String(rfCaptureFrequencyMhz, 2) +
              " timeout_ms=" + String(rfCaptureTimeoutMs));
  holdStatusLed(LedMode::RfCapture, rfCaptureTimeoutMs + 500);
}

void stopRfCapture(const String& status) {
  noInterrupts();
  rfCaptureArmed = false;
  rfCaptureActive = false;
  interrupts();
  restoreRadioTxMode();
  rfCaptureStatus = status;
  remoteLog("warn", String("RF capture stopped status=") + status);
}

void finalizeRfCapture() {
  size_t localCount = 0;
  bool localOverflow = false;

  detachInterrupt(digitalPinToInterrupt(RF_RX_PIN));
  noInterrupts();
  localCount = rfCapturePulseCount;
  if (localCount > RF_CAPTURE_MAX_PULSES) {
    localCount = RF_CAPTURE_MAX_PULSES;
  }
  for (size_t index = 0; index < localCount; ++index) {
    rfCaptureScratchPulses[index] = rfCapturePulses[index];
  }
  localOverflow = rfCaptureOverflow;
  rfCaptureActive = false;
  interrupts();

  if (localCount < 20) {
    rfCaptureIgnoredShortFrames++;
    resetRfCaptureState();
    rfCaptureStatus = "armed";
    attachInterrupt(digitalPinToInterrupt(RF_RX_PIN), onRfCaptureEdge, CHANGE);
    return;
  }

  noInterrupts();
  rfCaptureArmed = false;
  interrupts();
  restoreRadioTxMode();

  lastRfCaptureCount = localCount;
  for (size_t index = 0; index < localCount; ++index) {
    lastRfCapturePulses[index] = rfCaptureScratchPulses[index];
  }
  lastRfCaptureOverflow = localOverflow;
  lastRfCaptureRssi = ELECHOUSE_cc1101.getRssi();
  lastRfCaptureLqi = ELECHOUSE_cc1101.getLqi();
  lastRfCaptureMs = millis();
  lastRfCaptureSequence++;
  rfCaptureStatus = "captured";

  remoteLog("warn",
            String("RF captured label=") + rfCaptureLabel +
              " seq=" + String(lastRfCaptureSequence) +
              " count=" + String(lastRfCaptureCount) +
              " overflow=" + (lastRfCaptureOverflow ? "true" : "false") +
              " rssi=" + String(lastRfCaptureRssi));
}

void pollRfCapture() {
  if (!rfCaptureArmed) {
    return;
  }

  const bool frameEnded = rfCaptureActive && (micros() - rfCaptureLastFrameEdgeUs > RF_CAPTURE_IDLE_FRAME_US);
  if (frameEnded) {
    finalizeRfCapture();
    return;
  }

  if (millis() - rfCaptureStartedMs > rfCaptureTimeoutMs) {
    stopRfCapture(rfCaptureActive ? "timeout_active" : "timeout_no_signal");
  }
}

String bridgeDeviceJson() {
  String payload;
  payload.reserve(260);
  payload += "\"device\":{";
  payload += "\"identifiers\":[\"";
  payload += DEVICE_ID;
  payload += "\"],";
  payload += "\"name\":\"";
  payload += jsonEscape(DEVICE_NAME);
  payload += "\",";
  payload += "\"manufacturer\":\"DIY\",";
  payload += "\"model\":\"ESP32 CC1101 RF Blind Bridge\",";
  payload += "\"sw_version\":\"";
  payload += FIRMWARE_VERSION;
  payload += "\",";
  payload += "\"hw_version\":\"ESP32 + CC1101\"";
  payload += "}";
  return payload;
}

const char* stateForPosition(int position) {
  return position <= 0 ? "closed" : "open";
}

int estimatePosition(const BlindRuntime& state) {
  if (state.position < 0) {
    return -1;
  }
  if (state.motion == Motion::Stopped) {
    return state.position;
  }

  const uint32_t elapsedMs = millis() - state.motionStartedMs;
  const uint32_t travelMs = maxU32(state.definition->fullTravelMs, 1000);
  int delta = static_cast<int>((elapsedMs * 100UL) / travelMs);
  if (delta > 100) {
    delta = 100;
  }

  const int signedDelta = state.motion == Motion::Opening ? delta : -delta;
  return clampPosition(state.startPosition + signedDelta);
}

void recordCommandResult(
  BlindRuntime& state,
  const char* command,
  const char* result,
  const String& error = "",
  bool countCommand = true) {
  state.lastCommand = command;
  state.lastResult = result;
  state.lastError = error;
  state.lastCommandMs = millis();
  if (countCommand) {
    state.commandCount++;
    if (error.length() > 0 || strcmp(result, "failed") == 0 || strcmp(result, "ignored") == 0) {
      state.failedCommandCount++;
    }
  }
}

void publishCoverAttributes(BlindRuntime& state) {
  if (!mqtt.connected()) {
    return;
  }

  const BlindDefinition& blind = *state.definition;
  const int estimatedPosition = estimatePosition(state);
  String payload;
  payload.reserve(700);
  payload += "{";
  payload += "\"id\":\"";
  payload += jsonEscape(blind.id);
  payload += "\",";
  payload += "\"motion\":\"";
  payload += motionToString(state.motion);
  payload += "\",";
  payload += "\"position_known\":";
  payload += boolJson(estimatedPosition >= 0);
  payload += ",";
  payload += "\"estimated_position\":";
  payload += (estimatedPosition >= 0 ? String(estimatedPosition) : "null");
  payload += ",";
  payload += "\"target_position\":";
  payload += (state.targetPosition >= 0 ? String(state.targetPosition) : "null");
  payload += ",";
  payload += "\"full_travel_ms\":";
  payload += String(blind.fullTravelMs);
  payload += ",";
  payload += "\"invert_position\":";
  payload += boolJson(blind.invertPosition);
  payload += ",";
  payload += "\"rf_frequency_mhz\":";
  payload += String(RF_FREQUENCY_MHZ, 2);
  payload += ",";
  payload += "\"rf_open_configured\":";
  payload += boolJson(isRfConfigured(blind.openCommand));
  payload += ",";
  payload += "\"rf_close_configured\":";
  payload += boolJson(isRfConfigured(blind.closeCommand));
  payload += ",";
  payload += "\"rf_stop_configured\":";
  payload += boolJson(isRfConfigured(blind.stopCommand));
  payload += ",";
  payload += "\"last_command\":\"";
  payload += jsonEscape(state.lastCommand);
  payload += "\",";
  payload += "\"last_result\":\"";
  payload += jsonEscape(state.lastResult);
  payload += "\",";
  payload += "\"last_error\":\"";
  payload += jsonEscape(state.lastError);
  payload += "\",";
  payload += "\"command_count\":";
  payload += String(state.commandCount);
  payload += ",";
  payload += "\"failed_command_count\":";
  payload += String(state.failedCommandCount);
  payload += ",";
  payload += "\"last_command_ms\":";
  payload += (state.lastCommandMs > 0 ? String(state.lastCommandMs) : "null");
  payload += "}";

  mqtt.publish(blindTopic(blind, "attributes").c_str(), payload.c_str(), MQTT_RETAIN);
}

void publishCoverState(BlindRuntime& state, const char* forcedState = nullptr) {
  const BlindDefinition& blind = *state.definition;
  const int estimatedPosition = estimatePosition(state);
  const char* stateText = forcedState;

  if (stateText == nullptr) {
    if (state.motion == Motion::Opening) {
      stateText = "opening";
    } else if (state.motion == Motion::Closing) {
      stateText = "closing";
    } else if (estimatedPosition >= 0) {
      stateText = stateForPosition(estimatedPosition);
    } else {
      stateText = "stopped";
    }
  }

  mqtt.publish(blindTopic(blind, "state").c_str(), stateText, MQTT_RETAIN);
  if (estimatedPosition >= 0) {
    const String positionPayload = String(estimatedPosition);
    mqtt.publish(blindTopic(blind, "position").c_str(), positionPayload.c_str(), MQTT_RETAIN);
  }
  publishCoverAttributes(state);
}

String debugStatusJson() {
  String payload;
  payload.reserve(2500 + (BLIND_COUNT * 380));
  payload += "{";
  payload += "\"device_id\":\"";
  payload += DEVICE_ID;
  payload += "\",";
  payload += "\"device_name\":\"";
  payload += jsonEscape(DEVICE_NAME);
  payload += "\",";
  payload += "\"firmware\":\"";
  payload += FIRMWARE_VERSION;
  payload += "\",";
  payload += "\"boot_id\":\"";
  payload += bootIdText();
  payload += "\",";
  payload += "\"uptime_s\":";
  payload += String(millis() / 1000UL);
  payload += ",";
  payload += "\"time\":";
  const String nowText = wallClockReady() ? formatEpochLocal(time(nullptr)) : "";
  if (nowText.length() > 0) {
    payload += "\"";
    payload += nowText;
    payload += "\"";
  } else {
    payload += "null";
  }
  payload += ",";
  payload += "\"time_synced\":";
  payload += wallClockReady() ? "true" : "false";
  payload += ",";
  payload += "\"ip\":\"";
  payload += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(""));
  payload += "\",";
  payload += "\"wifi_status\":\"";
  payload += wifiStatusText(WiFi.status());
  payload += "\",";
  payload += "\"wifi_rssi\":";
  payload += (WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("null"));
  payload += ",";
  payload += "\"mqtt_connected\":";
  payload += (mqtt.connected() ? "true" : "false");
  payload += ",";
  payload += "\"mqtt_state\":";
  payload += String(mqtt.state());
  payload += ",";
  payload += "\"ota_ready\":";
  payload += (otaStarted ? "true" : "false");
  payload += ",";
  payload += "\"ota_hostname\":\"";
  payload += DEVICE_ID;
  payload += "\",";
  payload += "\"ota_port\":";
  payload += String(OTA_PORT);
  payload += ",";
  payload += "\"ota_in_progress\":";
  payload += (otaInProgress ? "true" : "false");
  payload += ",";
  payload += "\"http_update_message\":\"";
  payload += jsonEscape(httpUpdateMessage);
  payload += "\",";
  payload += "\"radio_ready\":";
  payload += (radioReady ? "true" : "false");
  payload += ",";
  payload += "\"rf_capture_status\":\"";
  payload += jsonEscape(rfCaptureStatus);
  payload += "\",";
  payload += "\"rf_capture_armed\":";
  payload += rfCaptureArmed ? "true" : "false";
  payload += ",";
  payload += "\"rf_capture_label\":\"";
  payload += jsonEscape(rfCaptureLabel);
  payload += "\",";
  payload += "\"rf_capture_sequence\":";
  payload += String(lastRfCaptureSequence);
  payload += ",";
  payload += "\"rf_capture_count\":";
  payload += String(lastRfCaptureCount);
  payload += ",";
  payload += "\"rf_capture_overflow\":";
  payload += lastRfCaptureOverflow ? "true" : "false";
  payload += ",";
  payload += "\"reboot_pending\":";
  payload += (rebootPending ? "true" : "false");
  payload += ",";
  payload += "\"led_status\":\"";
  payload += currentLedStatus;
  payload += "\",";
  payload += "\"free_heap\":";
  payload += String(ESP.getFreeHeap());
  payload += ",";
  payload += "\"log_sequence\":";
  payload += String(logSequence);
  payload += ",";
  payload += "\"log_buffer_size\":";
  payload += String(REMOTE_LOG_BUFFER_SIZE);
  payload += ",";
  payload += "\"log_count\":";
  payload += String(remoteLogCount);
  payload += ",";
  payload += "\"log_dropped\":";
  payload += String(remoteLogDroppedCount);
  payload += ",";
  payload += "\"last_log\":";
  if (remoteLogCount > 0) {
    const size_t lastIndex = (remoteLogStart + remoteLogCount - 1) % REMOTE_LOG_BUFFER_SIZE;
    payload += "\"";
    payload += jsonEscape(remoteLogLine(remoteLogs[lastIndex]));
    payload += "\"";
  } else {
    payload += "null";
  }
  payload += ",";
  payload += "\"blinds\":[";
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    BlindRuntime& state = blindStates[index];
    const BlindDefinition& blind = *state.definition;
    const int estimatedPosition = estimatePosition(state);
    if (index > 0) {
      payload += ",";
    }
    payload += "{";
    payload += "\"id\":\"";
    payload += jsonEscape(blind.id);
    payload += "\",";
    payload += "\"name\":\"";
    payload += jsonEscape(blind.name);
    payload += "\",";
    payload += "\"motion\":\"";
    payload += motionToString(state.motion);
    payload += "\",";
    payload += "\"position\":";
    payload += (estimatedPosition >= 0 ? String(estimatedPosition) : String("null"));
    payload += ",";
    payload += "\"last_command\":\"";
    payload += jsonEscape(state.lastCommand);
    payload += "\",";
    payload += "\"last_result\":\"";
    payload += jsonEscape(state.lastResult);
    payload += "\",";
    payload += "\"last_error\":\"";
    payload += jsonEscape(state.lastError);
    payload += "\",";
    payload += "\"command_count\":";
    payload += String(state.commandCount);
    payload += ",";
    payload += "\"failed_command_count\":";
    payload += String(state.failedCommandCount);
    payload += "}";
  }
  payload += "]}";
  return payload;
}

String debugLogsText() {
  String payload;
  payload.reserve(remoteLogCount * 120);
  for (size_t offset = 0; offset < remoteLogCount; ++offset) {
    const size_t index = (remoteLogStart + offset) % REMOTE_LOG_BUFFER_SIZE;
    payload += remoteLogLine(remoteLogs[index]);
    payload += "\n";
  }
  return payload;
}

String debugLogsJson() {
  String payload;
  payload.reserve(remoteLogCount * 220);
  payload += "[";
  for (size_t offset = 0; offset < remoteLogCount; ++offset) {
    const size_t index = (remoteLogStart + offset) % REMOTE_LOG_BUFFER_SIZE;
    if (offset > 0) {
      payload += ",";
    }
    payload += remoteLogJson(remoteLogs[index]);
  }
  payload += "]";
  return payload;
}

void handleDebugRoot() {
  String payload;
  payload.reserve(9000);
  payload += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  payload += "<title>";
  payload += DEVICE_NAME;
  payload += "</title><style>body{font-family:system-ui,sans-serif;margin:24px;max-width:960px;line-height:1.45}code,pre{background:#f3f3f3;padding:2px 4px}pre{padding:12px;overflow:auto}.ok{color:#067d17}.bad{color:#b00020}.hint{color:#555;max-width:760px}.section{border-top:1px solid #ddd;margin-top:24px;padding-top:14px}.field{display:inline-block;margin:6px 8px 6px 0}.field span{display:block;font-size:.86rem;color:#555}input{font:inherit;padding:7px}button{font:inherit;padding:10px 14px;border:1px solid #666;border-radius:6px;background:#fff;cursor:pointer}button.danger{border-color:#b00020;color:#b00020}ul.links li{margin:6px 0}table{border-collapse:collapse;margin:10px 0 4px;max-width:760px}th,td{border:1px solid #ddd;padding:7px 9px;text-align:left;vertical-align:top}th{background:#f7f7f7}</style></head><body>";
  payload += "<h1>";
  payload += DEVICE_NAME;
  payload += "</h1>";
  payload += "<p class=\"hint\">This is the ESP32 bridge that Home Assistant uses to send RF commands to the blinds. For normal blind control, use Home Assistant. This page is for status checks, learning remote buttons, and rebooting.</p>";
  payload += "<p><strong>Firmware:</strong> ";
  payload += FIRMWARE_VERSION;
  payload += " &nbsp; <strong>Boot ID:</strong> ";
  payload += bootIdText();
  payload += "</p>";
  payload += "<p><strong>Bridge status:</strong> <span class=\"";
  payload += (radioReady && WiFi.status() == WL_CONNECTED ? "ok" : "bad");
  payload += "\">";
  payload += currentLedStatus;
  payload += "</span></p>";
  payload += "<p class=\"hint\">Status mirrors the RGB LED. <strong>online</strong> means WiFi, MQTT, and the CC1101 radio are ready. Yellow blinking means the bridge is listening for a remote button during RF capture.</p>";
  payload += "<table><thead><tr><th>RGB LED</th><th>What it means</th></tr></thead><tbody>";
  payload += "<tr><td>Dim blue</td><td>Starting up.</td></tr>";
  payload += "<tr><td>Amber blinking</td><td>Connecting to WiFi.</td></tr>";
  payload += "<tr><td>Blue blinking</td><td>WiFi is connected; connecting to Home Assistant/MQTT.</td></tr>";
  payload += "<tr><td>Solid green</td><td>Ready. WiFi, MQTT, and the CC1101 radio are working.</td></tr>";
  payload += "<tr><td>White</td><td>Sending a blind command.</td></tr>";
  payload += "<tr><td>Yellow blinking</td><td>Listening for one physical remote button during RF capture.</td></tr>";
  payload += "<tr><td>Cyan blinking</td><td>Firmware is being uploaded.</td></tr>";
  payload += "<tr><td>Purple</td><td>Reboot was requested.</td></tr>";
  payload += "<tr><td>Red blinking or solid red</td><td>CC1101 radio is missing or another error needs attention.</td></tr>";
  payload += "</tbody></table>";
  payload += "<div class=\"section\"><h2>Troubleshooting Links</h2><p class=\"hint\">These links are for debugging. They do not move the blinds.</p><ul class=\"links\"><li><a href=\"/status\">Status details</a> - WiFi, MQTT, radio, firmware, and blind state as JSON.</li><li><a href=\"/logs\">Recent log</a> - plain text events from this bridge.</li><li><a href=\"/logs.json\">Recent log as JSON</a> - same log in machine-readable form.</li></ul></div>";
  payload += "<div class=\"section\"><h2>RF Capture</h2><p class=\"hint\">Use this only when teaching the ESP32 a physical remote button. Click <strong>Start listening</strong>, press exactly one button on the remote once, then read the saved RAW/JSON result. Home Assistant blind commands are blocked while this is listening.</p><p><strong>Capture status:</strong> ";
  payload += rfCaptureStatus;
  payload += "";
  if (lastRfCaptureCount > 0) {
    payload += " · last ";
    payload += String(lastRfCaptureCount);
    payload += " pulses";
  }
  payload += "</p>";
  payload += "<form method=\"post\" action=\"/capture/start\"><label class=\"field\"><span>Name this button press</span><input name=\"label\" placeholder=\"last_blind_up\" required></label>";
  payload += "<label class=\"field\"><span>Radio frequency (MHz)</span><input name=\"freq\" value=\"";
  payload += String(RF_FREQUENCY_MHZ, 2);
  payload += "\" size=\"6\"></label>";
  payload += "<label class=\"field\"><span>Listen time (seconds)</span><input name=\"seconds\" value=\"15\" size=\"3\"></label>";
  payload += "<button type=\"submit\">Start listening</button></form>";
  payload += "<p><a href=\"/capture\">Open the detailed capture page</a> · <a href=\"/capture.raw\">RAW capture text</a> · <a href=\"/capture.json\">JSON capture data</a></p></div>";
  payload += "<div class=\"section\"><h2>Actions</h2><p class=\"hint\">Reboot only restarts this ESP32 bridge. It does not restart Home Assistant and it does not erase settings.</p><form method=\"post\" action=\"/reboot\" onsubmit=\"return confirm('Reboot the ESP32 now?');\"><button class=\"danger\" type=\"submit\">Reboot ESP32</button></form></div>";
  payload += "<div class=\"section\"><h2>Recent Logs</h2><p class=\"hint\">Recent events from the ESP32. Each line starts with local date/time, then milliseconds since boot, then the event. This helps diagnose WiFi, MQTT, RF capture, and command problems.</p><pre>";
  payload += jsonEscape(debugLogsText());
  payload += "</pre></div></body></html>";
  debugServer.send(200, "text/html", payload);
}

void handleDebugStatus() {
  debugServer.send(200, "application/json", debugStatusJson());
}

void handleDebugLogs() {
  debugServer.send(200, "text/plain", debugLogsText());
}

void handleDebugLogsJson() {
  debugServer.send(200, "application/json", debugLogsJson());
}

void handleDebugCapturePage() {
  String payload;
  payload.reserve(3600 + (lastRfCaptureCount * 8));
  payload += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  payload += "<meta http-equiv=\"refresh\" content=\"3\"><title>RF Capture ";
  payload += DEVICE_NAME;
  payload += "</title><style>body{font-family:system-ui,sans-serif;margin:24px;max-width:960px;line-height:1.45}.hint{color:#555;max-width:760px}.field{display:inline-block;margin:6px 10px 6px 0}.field span{display:block;font-size:.86rem;color:#555}input,button{font:inherit;padding:8px}button{border:1px solid #666;border-radius:6px;background:#fff;cursor:pointer}.section{border-top:1px solid #ddd;margin-top:24px;padding-top:14px}pre{background:#f3f3f3;padding:12px;overflow:auto;white-space:pre-wrap}</style></head><body>";
  payload += "<h1>Remote Button Capture</h1>";
  payload += "<p class=\"hint\">This page records the RF signal from one physical remote button. Use it when a blind does not respond correctly and we need to relearn UP, STOP, or DOWN.</p>";
  payload += "<p><strong>Capture status:</strong> ";
  payload += rfCaptureStatus;
  if (rfCaptureArmed) {
    payload += " - listening now";
  }
  payload += "</p>";
  payload += "<p class=\"hint\">When listening, the RGB LED blinks yellow and Home Assistant RF commands are paused. Press exactly one remote button once, then wait for the status to change to captured.</p>";
  payload += "<div class=\"section\"><h2>Start A Capture</h2><form method=\"post\" action=\"/capture/start\"><label class=\"field\"><span>Name this button press</span><input name=\"label\" placeholder=\"last_blind_up\" value=\"";
  payload += jsonEscape(rfCaptureLabel);
  payload += "\" required></label><label class=\"field\"><span>Radio frequency (leave at 433.92)</span><input name=\"freq\" value=\"";
  payload += String(rfCaptureFrequencyMhz, 2);
  payload += "\" size=\"6\"></label><label class=\"field\"><span>How long to listen</span><input name=\"seconds\" value=\"15\" size=\"3\"></label><button type=\"submit\">Start listening</button></form>";
  payload += "<form method=\"post\" action=\"/capture/stop\"><button type=\"submit\">Stop listening</button></form></div>";
  payload += "<div class=\"section\"><h2>Saved Result</h2><p class=\"hint\"><strong>RAW capture text</strong> is what we save into the repo and decode into firmware commands. <strong>JSON capture data</strong> is the same result in a structured format for tools.</p>";
  payload += "<p><a href=\"/\">Back to bridge home</a> · <a href=\"/capture.raw\">RAW capture text</a> · <a href=\"/capture.json\">JSON capture data</a></p>";
  payload += "<h3>Last Capture</h3><pre>";
  payload += jsonEscape(rfCaptureRawText());
  payload += "</pre></div></body></html>";
  debugServer.send(200, "text/html", payload);
}

void handleDebugCaptureStart() {
  String label = debugServer.arg("label");
  label.trim();
  float frequencyMhz = debugServer.hasArg("freq") ? debugServer.arg("freq").toFloat() : RF_FREQUENCY_MHZ;
  if (frequencyMhz < 300.0f || frequencyMhz > 928.0f) {
    frequencyMhz = RF_FREQUENCY_MHZ;
  }

  uint32_t seconds = debugServer.hasArg("seconds") ? static_cast<uint32_t>(debugServer.arg("seconds").toInt()) : 15;
  if (seconds < 3) {
    seconds = 3;
  }
  if (seconds > 60) {
    seconds = 60;
  }

  beginRfCapture(label, frequencyMhz, seconds * 1000UL);
  debugServer.sendHeader("Location", "/capture");
  debugServer.send(303, "text/plain", "Capture armed\n");
}

void handleDebugCaptureStop() {
  if (rfCaptureArmed) {
    stopRfCapture("stopped_by_http");
  }
  debugServer.sendHeader("Location", "/capture");
  debugServer.send(303, "text/plain", "Capture stopped\n");
}

void handleDebugCaptureRaw() {
  debugServer.send(200, "text/plain", rfCaptureRawText());
}

void handleDebugCaptureJson() {
  debugServer.send(200, "application/json", rfCaptureJson());
}

void handleDebugRebootConfirm() {
  String payload;
  payload.reserve(900);
  payload += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  payload += "<title>Reboot ";
  payload += DEVICE_NAME;
  payload += "</title><style>body{font-family:system-ui,sans-serif;margin:24px;max-width:640px}button,a{font:inherit;display:inline-block;margin-right:12px;padding:10px 14px;border:1px solid #666;border-radius:6px;background:#fff;color:#111;text-decoration:none}button.danger{border-color:#b00020;color:#b00020}</style></head><body>";
  payload += "<h1>Reboot ESP32?</h1><p>The bridge will disconnect briefly while it restarts.</p>";
  payload += "<form method=\"post\" action=\"/reboot\"><button class=\"danger\" type=\"submit\">Reboot ESP32</button><a href=\"/\">Cancel</a></form>";
  payload += "</body></html>";
  debugServer.send(200, "text/html", payload);
}

void handleDebugReboot() {
  rebootPending = true;
  rebootAtMs = millis() + 1500UL;
  remoteLog("warn", "HTTP reboot requested");
  holdStatusLed(LedMode::Rebooting, 2000);

  String payload;
  payload.reserve(700);
  payload += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  payload += "<meta http-equiv=\"refresh\" content=\"8;url=/\"><title>Rebooting ";
  payload += DEVICE_NAME;
  payload += "</title><style>body{font-family:system-ui,sans-serif;margin:24px;max-width:640px}</style></head><body>";
  payload += "<h1>Rebooting ESP32</h1><p>This page will try to reload shortly.</p>";
  payload += "</body></html>";
  debugServer.send(200, "text/html", payload);
}

void handleDebugUpdateResult() {
  if (httpUpdateOk) {
    rebootPending = true;
    rebootAtMs = millis() + 1500UL;
    holdStatusLed(LedMode::Rebooting, 2000);

    String payload;
    payload.reserve(800);
    payload += "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    payload += "<meta http-equiv=\"refresh\" content=\"10;url=/\"><title>Firmware Updated</title>";
    payload += "<style>body{font-family:system-ui,sans-serif;margin:24px;max-width:640px}</style></head><body>";
    payload += "<h1>Firmware Updated</h1><p>";
    payload += jsonEscape(httpUpdateMessage);
    payload += "</p><p>The ESP32 is rebooting now.</p></body></html>";
    debugServer.send(200, "text/html", payload);
    return;
  }

  String payload;
  payload.reserve(500);
  payload += "Firmware update failed: ";
  payload += httpUpdateMessage.length() > 0 ? httpUpdateMessage : String("unknown error");
  payload += "\n";
  debugServer.send(500, "text/plain", payload);
}

void handleDebugUpdateUpload() {
  HTTPUpload& upload = debugServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    httpUpdateOk = false;
    httpUpdateMessage = "";
    otaInProgress = true;
    remoteLog("warn", String("HTTP firmware update started file=") + upload.filename);
    holdStatusLed(LedMode::OtaUpdate, 120000);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      httpUpdateMessage = String("Update.begin failed: ") + Update.errorString();
      remoteLog("error", httpUpdateMessage);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.hasError()) {
      return;
    }
    const size_t written = Update.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      httpUpdateMessage = String("Update.write failed: ") + Update.errorString();
      remoteLog("error", httpUpdateMessage);
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    otaInProgress = false;
    if (!Update.hasError() && Update.end(true)) {
      httpUpdateOk = true;
      httpUpdateMessage = String("HTTP firmware update complete bytes=") + String(upload.totalSize);
      remoteLog("warn", httpUpdateMessage);
      return;
    }
    httpUpdateMessage = String("Update.end failed: ") + Update.errorString();
    remoteLog("error", httpUpdateMessage);
    holdStatusLed(LedMode::Error, 5000);
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    otaInProgress = false;
    Update.abort();
    httpUpdateMessage = "HTTP firmware update aborted";
    remoteLog("error", httpUpdateMessage);
    holdStatusLed(LedMode::Error, 5000);
  }
}

void handleDebugNotFound() {
  debugServer.send(404, "text/plain", "Not found. Use /status, /logs, /logs.json, /capture, /capture.raw, /reboot, or POST /update.\n");
}

void configureDebugServer() {
  debugServer.on("/", HTTP_GET, handleDebugRoot);
  debugServer.on("/status", HTTP_GET, handleDebugStatus);
  debugServer.on("/logs", HTTP_GET, handleDebugLogs);
  debugServer.on("/logs.json", HTTP_GET, handleDebugLogsJson);
  debugServer.on("/capture", HTTP_GET, handleDebugCapturePage);
  debugServer.on("/capture/start", HTTP_POST, handleDebugCaptureStart);
  debugServer.on("/capture/stop", HTTP_POST, handleDebugCaptureStop);
  debugServer.on("/capture.raw", HTTP_GET, handleDebugCaptureRaw);
  debugServer.on("/capture.json", HTTP_GET, handleDebugCaptureJson);
  debugServer.on("/reboot", HTTP_GET, handleDebugRebootConfirm);
  debugServer.on("/reboot", HTTP_POST, handleDebugReboot);
  debugServer.on("/update", HTTP_POST, handleDebugUpdateResult, handleDebugUpdateUpload);
  debugServer.onNotFound(handleDebugNotFound);
}

void ensureDebugServerStarted() {
  if (debugServerStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }
  debugServer.begin();
  debugServerStarted = true;
  remoteLog("info", String("HTTP debug server ready at http://") + WiFi.localIP().toString() + "/");
}

void ensureOtaStarted() {
  if (otaStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPort(OTA_PORT);
  if (strlen(OTA_PASSWORD) > 0) {
    ArduinoOTA.setPassword(OTA_PASSWORD);
  }

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    remoteLog("warn", "OTA update started");
    holdStatusLed(LedMode::OtaUpdate, 120000);
  });

  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    remoteLog("warn", "OTA update finished; rebooting");
    holdStatusLed(LedMode::Rebooting, 5000);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    remoteLog("error", String("OTA update failed error=") + otaErrorText(error));
    holdStatusLed(LedMode::Error, 5000);
  });

  ArduinoOTA.begin();
  otaStarted = true;
  remoteLog("info", String("OTA ready hostname=") + DEVICE_ID + " port=" + String(OTA_PORT));
}

void restartIfRequested() {
  if (!rebootPending) {
    return;
  }
  if (millis() < rebootAtMs) {
    return;
  }
  remoteLog("warn", "Restarting ESP32 now");
  delay(50);
  ESP.restart();
}

bool sendRfCommand(const BlindDefinition& blind, const RawRfCommand& command, String* error = nullptr) {
  if (rfCaptureArmed) {
    const String message = "RF capture is armed; transmit is blocked";
    if (error != nullptr) {
      *error = message;
    }
    remoteLog("warn", String("[") + blind.id + "] " + message + "; cannot send " + command.label);
    return false;
  }
  if (!radioReady) {
    const String message = "CC1101 radio is not ready";
    if (error != nullptr) {
      *error = message;
    }
    Serial.printf("[%s] %s; cannot send %s\n", blind.id, message.c_str(), command.label);
    remoteLog("error", String("[") + blind.id + "] " + message + "; cannot send " + command.label);
    holdStatusLed(LedMode::Error, 1500);
    return false;
  }
  if (!isRfConfigured(command)) {
    const String message = String("RF command '") + command.label + "' has no pulse data yet";
    if (error != nullptr) {
      *error = message;
    }
    Serial.printf("[%s] %s\n", blind.id, message.c_str());
    remoteLog("error", String("[") + blind.id + "] " + message);
    holdStatusLed(LedMode::Error, 1500);
    return false;
  }

  Serial.printf("[%s] sending RF command '%s' (%u pulses, %u repeats)\n",
                blind.id,
                command.label,
                static_cast<unsigned>(command.pulseCount),
                static_cast<unsigned>(command.repeats));
  remoteLog("info",
            String("[") + blind.id + "] RF " + command.label +
              " pulses=" + String(static_cast<unsigned>(command.pulseCount)) +
              " repeats=" + String(static_cast<unsigned>(command.repeats)));
  holdStatusLed(LedMode::RfTransmit, 750);

  ELECHOUSE_cc1101.SetTx(RF_FREQUENCY_MHZ);
  for (uint8_t repeat = 0; repeat < command.repeats; ++repeat) {
    bool level = true;
    for (size_t index = 0; index < command.pulseCount; ++index) {
      digitalWrite(RF_TX_PIN, level ? HIGH : LOW);
      delayMicroseconds(command.pulses[index]);
      level = !level;
    }
    digitalWrite(RF_TX_PIN, LOW);
    delay(command.interFrameGapMs);
  }

  remoteLog("info", String("[") + blind.id + "] RF " + command.label + " sent");
  return true;
}

void stopBlind(BlindRuntime& state, bool sendRf) {
  const BlindDefinition& blind = *state.definition;
  String error;
  if (sendRf && !sendRfCommand(blind, blind.stopCommand, &error)) {
    recordCommandResult(state, "stop", "failed", error);
    publishCoverAttributes(state);
    return;
  }

  const int estimatedPosition = estimatePosition(state);
  if (estimatedPosition >= 0) {
    state.position = estimatedPosition;
  }
  state.motion = Motion::Stopped;
  state.targetPosition = state.position;
  recordCommandResult(state, "stop", sendRf ? "sent" : "synced");
  publishCoverState(state, "stopped");
}

bool startMoveTo(BlindRuntime& state, int targetPosition) {
  const BlindDefinition& blind = *state.definition;
  targetPosition = clampPosition(targetPosition);

  int currentPosition = estimatePosition(state);
  if (currentPosition < 0) {
    if (targetPosition == 100) {
      currentPosition = 0;
    } else if (targetPosition == 0) {
      currentPosition = 100;
    } else {
      const String error = "unknown current position; run full open or close before intermediate positions";
      Serial.printf("[%s] %s: %d\n", blind.id, error.c_str(), targetPosition);
      recordCommandResult(state, "set_position", "failed", error);
      publishCoverAttributes(state);
      return false;
    }
  }

  if (targetPosition == currentPosition) {
    state.position = currentPosition;
    state.motion = Motion::Stopped;
    recordCommandResult(state, "set_position", "unchanged");
    publishCoverState(state);
    return true;
  }

  const Motion nextMotion = targetPosition > currentPosition ? Motion::Opening : Motion::Closing;
  const RawRfCommand& command = nextMotion == Motion::Opening ? blind.openCommand : blind.closeCommand;
  if (targetPosition > 0 && targetPosition < 100 && !isRfConfigured(blind.stopCommand)) {
    const String error = "intermediate position requires a configured stop RF command";
    Serial.printf("[%s] %s\n", blind.id, error.c_str());
    recordCommandResult(state, "set_position", "failed", error);
    publishCoverAttributes(state);
    return false;
  }

  String error;
  if (!sendRfCommand(blind, command, &error)) {
    recordCommandResult(state, command.label, "failed", error);
    publishCoverAttributes(state);
    return false;
  }

  state.position = currentPosition;
  state.startPosition = currentPosition;
  state.targetPosition = targetPosition;
  state.motion = nextMotion;
  state.motionStartedMs = millis();

  const uint32_t percentDelta = static_cast<uint32_t>(abs(targetPosition - currentPosition));
  state.motionDurationMs = maxU32((maxU32(blind.fullTravelMs, 1000) * percentDelta) / 100UL, 250);
  recordCommandResult(state, command.label, "sent");
  publishCoverState(state);
  return true;
}

bool parsePositionPayload(const String& payload, int& position) {
  if (payload.length() == 0) {
    return false;
  }
  for (size_t index = 0; index < payload.length(); ++index) {
    const char ch = payload[index];
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  position = clampPosition(payload.toInt());
  return true;
}

void handleCommand(BlindRuntime& state, String payload) {
  payload.trim();
  payload.toUpperCase();

  int requestedPosition = -1;
  if (payload == "OPEN") {
    requestedPosition = 100;
  } else if (payload == "CLOSE") {
    requestedPosition = 0;
  } else if (payload == "STOP") {
    stopBlind(state, true);
    return;
  } else if (payload == "SYNC_OPEN") {
    state.position = 100;
    state.startPosition = 100;
    state.targetPosition = 100;
    state.motion = Motion::Stopped;
    recordCommandResult(state, "sync_open", "synced");
    publishCoverState(state);
    return;
  } else if (payload == "SYNC_CLOSE") {
    state.position = 0;
    state.startPosition = 0;
    state.targetPosition = 0;
    state.motion = Motion::Stopped;
    recordCommandResult(state, "sync_close", "synced");
    publishCoverState(state);
    return;
  } else if (payload == "STATUS") {
    publishCoverState(state);
    return;
  } else if (!parsePositionPayload(payload, requestedPosition)) {
    const String error = String("ignored MQTT payload: ") + payload;
    Serial.printf("[%s] %s\n", state.definition->id, error.c_str());
    recordCommandResult(state, "unknown", "ignored", error);
    publishCoverAttributes(state);
    return;
  }

  startMoveTo(state, requestedPosition);
}

void handleMqttMessage(char* topic, byte* payload, unsigned int length) {
  String topicText(topic);
  String payloadText;
  payloadText.reserve(length + 1);
  for (unsigned int index = 0; index < length; ++index) {
    payloadText += static_cast<char>(payload[index]);
  }

  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    BlindRuntime& state = blindStates[index];
    const BlindDefinition& blind = *state.definition;
    if (topicText == blindTopic(blind, "set") || topicText == blindTopic(blind, "set_position")) {
      handleCommand(state, payloadText);
      return;
    }
  }
}

void publishDiscovery(const BlindDefinition& blind) {
  String payload;
  payload.reserve(1300);
  payload += "{";
  payload += "\"name\":\"";
  payload += jsonEscape(blind.name);
  payload += "\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_";
  payload += blind.id;
  payload += "\",";
  payload += "\"command_topic\":\"";
  payload += blindTopic(blind, "set");
  payload += "\",";
  payload += "\"payload_open\":\"OPEN\",";
  payload += "\"payload_close\":\"CLOSE\",";
  payload += "\"payload_stop\":\"STOP\",";
  payload += "\"state_topic\":\"";
  payload += blindTopic(blind, "state");
  payload += "\",";
  payload += "\"state_open\":\"open\",";
  payload += "\"state_opening\":\"opening\",";
  payload += "\"state_closed\":\"closed\",";
  payload += "\"state_closing\":\"closing\",";
  payload += "\"state_stopped\":\"stopped\",";
  payload += "\"position_topic\":\"";
  payload += blindTopic(blind, "position");
  payload += "\",";
  payload += "\"set_position_topic\":\"";
  payload += blindTopic(blind, "set_position");
  payload += "\",";
  payload += "\"position_open\":100,";
  payload += "\"position_closed\":0,";
  payload += "\"json_attributes_topic\":\"";
  payload += blindTopic(blind, "attributes");
  payload += "\",";
  payload += "\"availability_topic\":\"";
  payload += bridgeAvailabilityTopic();
  payload += "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"device_class\":\"shade\",";
  // The motors provide no position feedback. Mark the entity as assumed-state so
  // Home Assistant never disables Open or Close based on our timing estimate.
  payload += "\"optimistic\":true,";
  payload += "\"retain\":false,";
  payload += bridgeDeviceJson();
  payload += "}";

  mqtt.publish(discoveryTopic(blind).c_str(), payload.c_str(), true);
}

void publishSensorDiscovery(
  const char* objectId,
  const char* name,
  const String& stateTopic,
  const char* icon,
  const char* unit = nullptr,
  const char* deviceClass = nullptr,
  const char* stateClass = nullptr) {
  String payload;
  payload.reserve(850);
  payload += "{";
  payload += "\"name\":\"";
  payload += jsonEscape(name);
  payload += "\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_";
  payload += objectId;
  payload += "\",";
  payload += "\"state_topic\":\"";
  payload += stateTopic;
  payload += "\",";
  payload += "\"availability_topic\":\"";
  payload += bridgeAvailabilityTopic();
  payload += "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"entity_category\":\"diagnostic\",";
  if (icon != nullptr && strlen(icon) > 0) {
    payload += "\"icon\":\"";
    payload += icon;
    payload += "\",";
  }
  if (unit != nullptr && strlen(unit) > 0) {
    payload += "\"unit_of_measurement\":\"";
    payload += unit;
    payload += "\",";
  }
  if (deviceClass != nullptr && strlen(deviceClass) > 0) {
    payload += "\"device_class\":\"";
    payload += deviceClass;
    payload += "\",";
  }
  if (stateClass != nullptr && strlen(stateClass) > 0) {
    payload += "\"state_class\":\"";
    payload += stateClass;
    payload += "\",";
  }
  payload += bridgeDeviceJson();
  payload += "}";
  mqtt.publish(entityDiscoveryTopic("sensor", objectId).c_str(), payload.c_str(), true);
}

void publishBinarySensorDiscovery(
  const char* objectId,
  const char* name,
  const String& stateTopic,
  const char* deviceClass,
  const char* payloadOn,
  const char* payloadOff) {
  String payload;
  payload.reserve(850);
  payload += "{";
  payload += "\"name\":\"";
  payload += jsonEscape(name);
  payload += "\",";
  payload += "\"unique_id\":\"";
  payload += DEVICE_ID;
  payload += "_";
  payload += objectId;
  payload += "\",";
  payload += "\"state_topic\":\"";
  payload += stateTopic;
  payload += "\",";
  payload += "\"payload_on\":\"";
  payload += payloadOn;
  payload += "\",";
  payload += "\"payload_off\":\"";
  payload += payloadOff;
  payload += "\",";
  payload += "\"availability_topic\":\"";
  payload += bridgeAvailabilityTopic();
  payload += "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"entity_category\":\"diagnostic\",";
  if (deviceClass != nullptr && strlen(deviceClass) > 0) {
    payload += "\"device_class\":\"";
    payload += deviceClass;
    payload += "\",";
  }
  payload += bridgeDeviceJson();
  payload += "}";
  mqtt.publish(entityDiscoveryTopic("binary_sensor", objectId).c_str(), payload.c_str(), true);
}

void publishDiagnosticDiscovery() {
  publishSensorDiscovery(
    "wifi_rssi",
    "WiFi RSSI",
    diagnosticTopic("wifi_rssi"),
    "mdi:wifi",
    "dBm",
    "signal_strength",
    "measurement");
  publishSensorDiscovery(
    "uptime",
    "Uptime",
    diagnosticTopic("uptime"),
    "mdi:timer-outline",
    "s");
  publishSensorDiscovery(
    "ip_address",
    "IP Address",
    diagnosticTopic("ip_address"),
    "mdi:ip-network");
  publishSensorDiscovery(
    "free_heap",
    "Free Heap",
    diagnosticTopic("free_heap"),
    "mdi:memory",
    "B",
    nullptr,
    "measurement");
  publishSensorDiscovery(
    "firmware_version",
    "Firmware Version",
    diagnosticTopic("firmware_version"),
    "mdi:chip");
  publishSensorDiscovery(
    "led_status",
    "LED Status",
    diagnosticTopic("led_status"),
    "mdi:led-on");
  publishSensorDiscovery(
    "debug_url",
    "Debug URL",
    diagnosticTopic("debug_url"),
    "mdi:web");
  publishSensorDiscovery(
    "last_log",
    "Last Log",
    diagnosticTopic("last_log"),
    "mdi:text-box-search-outline");
  publishSensorDiscovery(
    "log_sequence",
    "Log Sequence",
    diagnosticTopic("log_sequence"),
    "mdi:counter",
    nullptr,
    nullptr,
    "measurement");
  publishSensorDiscovery(
    "log_dropped",
    "Dropped Logs",
    diagnosticTopic("log_dropped"),
    "mdi:counter",
    nullptr,
    nullptr,
    "measurement");
  publishBinarySensorDiscovery(
    "radio_ready",
    "CC1101 Radio",
    diagnosticTopic("radio_ready"),
    "connectivity",
    "ready",
    "missing");
}

void publishAllDiscovery() {
  publishDiagnosticDiscovery();
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    publishDiscovery(*blindStates[index].definition);
  }
}

void subscribeBlindTopics() {
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    const BlindDefinition& blind = *blindStates[index].definition;
    mqtt.subscribe(blindTopic(blind, "set").c_str());
    mqtt.subscribe(blindTopic(blind, "set_position").c_str());
  }
}

void clearRetainedBlindCommands() {
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    const BlindDefinition& blind = *blindStates[index].definition;
    mqtt.publish(blindTopic(blind, "set").c_str(), "", true);
    mqtt.publish(blindTopic(blind, "set_position").c_str(), "", true);
  }
}

void publishAllStates() {
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    publishCoverState(blindStates[index]);
  }
}

void publishDiagnostics() {
  if (!mqtt.connected()) {
    return;
  }

  mqtt.publish(diagnosticTopic("wifi_rssi").c_str(), String(WiFi.RSSI()).c_str(), MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("uptime").c_str(), String(millis() / 1000UL).c_str(), MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("ip_address").c_str(), WiFi.localIP().toString().c_str(), MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("free_heap").c_str(), String(ESP.getFreeHeap()).c_str(), MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("firmware_version").c_str(), FIRMWARE_VERSION, MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("radio_ready").c_str(), radioReady ? "ready" : "missing", MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("led_status").c_str(), currentLedStatus.c_str(), MQTT_RETAIN);
  const String debugUrl = WiFi.status() == WL_CONNECTED ? String("http://") + WiFi.localIP().toString() + "/" : "";
  mqtt.publish(diagnosticTopic("debug_url").c_str(), debugUrl.c_str(), MQTT_RETAIN);
  if (remoteLogCount > 0) {
    const size_t lastIndex = (remoteLogStart + remoteLogCount - 1) % REMOTE_LOG_BUFFER_SIZE;
    const String lastLog = compactSensorText(remoteLogLine(remoteLogs[lastIndex]));
    mqtt.publish(diagnosticTopic("last_log").c_str(), lastLog.c_str(), MQTT_RETAIN);
    mqtt.publish(diagnosticTopic("last_log_level").c_str(), remoteLogs[lastIndex].level.c_str(), MQTT_RETAIN);
    mqtt.publish(diagnosticTopic("last_log_ms").c_str(), String(remoteLogs[lastIndex].ms).c_str(), MQTT_RETAIN);
  }
  mqtt.publish(diagnosticTopic("log_sequence").c_str(), String(logSequence).c_str(), MQTT_RETAIN);
  mqtt.publish(diagnosticTopic("log_dropped").c_str(), String(remoteLogDroppedCount).c_str(), MQTT_RETAIN);
}

void publishDiagnosticsIfDue() {
  const uint32_t now = millis();
  if (now - lastDiagnosticsPublishMs < DIAGNOSTICS_PUBLISH_INTERVAL_MS) {
    return;
  }
  lastDiagnosticsPublishMs = now;
  publishDiagnostics();
}

bool connectMqtt() {
  const String availability = bridgeAvailabilityTopic();
  bool connected = false;
  if (strlen(MQTT_USERNAME) > 0) {
    connected = mqtt.connect(
      DEVICE_ID,
      MQTT_USERNAME,
      MQTT_PASSWORD,
      availability.c_str(),
      0,
      true,
      "offline");
  } else {
    connected = mqtt.connect(
      DEVICE_ID,
      availability.c_str(),
      0,
      true,
      "offline");
  }

  if (!connected) {
    Serial.printf("MQTT connect failed, state=%d\n", mqtt.state());
    remoteLog("warn", String("MQTT connect failed state=") + String(mqtt.state()));
    return false;
  }

  mqtt.publish(availability.c_str(), "online", true);
  publishAllDiscovery();
  publishPendingRemoteLogsToMqtt();
  clearRetainedBlindCommands();
  subscribeBlindTopics();
  publishAllStates();
  publishDiagnostics();
  Serial.println("MQTT connected");
  remoteLog("info", "MQTT connected");
  return true;
}

void ensureMqttConnected() {
  if (mqtt.connected()) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  const uint32_t now = millis();
  if (lastMqttAttemptMs != 0 && now - lastMqttAttemptMs < MQTT_RECONNECT_INTERVAL_MS) {
    return;
  }
  lastMqttAttemptMs = now;
  connectMqtt();
}

void updateMotion() {
  const uint32_t now = millis();
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    BlindRuntime& state = blindStates[index];
    if (state.motion == Motion::Stopped) {
      continue;
    }

    if (now - state.lastPositionPublishMs >= POSITION_PUBLISH_INTERVAL_MS) {
      state.lastPositionPublishMs = now;
      publishCoverState(state);
    }

    if (now - state.motionStartedMs < state.motionDurationMs) {
      continue;
    }

    const bool intermediateStop = state.targetPosition > 0 && state.targetPosition < 100;
    bool reachedTarget = true;
    if (intermediateStop) {
      String error;
      if (sendRfCommand(*state.definition, state.definition->stopCommand, &error)) {
        recordCommandResult(state, "auto_stop", "sent", "", false);
      } else {
        recordCommandResult(state, "auto_stop", "failed", error, false);
        reachedTarget = false;
      }
    }

    state.position = reachedTarget ? state.targetPosition : -1;
    state.targetPosition = state.position;
    state.motion = Motion::Stopped;
    if (reachedTarget) {
      recordCommandResult(state, "move_complete", "completed", "", false);
      publishCoverState(state, stateForPosition(state.position));
    } else {
      publishCoverState(state, "stopped");
    }
  }
}

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (lastWifiAttemptMs != 0 && now - lastWifiAttemptMs < WIFI_RECONNECT_INTERVAL_MS) {
    return;
  }

  lastWifiAttemptMs = now;
  Serial.printf("Connecting WiFi SSID=%s\n", WIFI_SSID);
  remoteLog("info", String("Connecting WiFi SSID=") + WIFI_SSID);
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void logWifiStatusIfChanged() {
  const wl_status_t status = WiFi.status();
  if (status == lastLoggedWifiStatus) {
    return;
  }
  lastLoggedWifiStatus = status;
  String message = String("WiFi status ") + wifiStatusText(status);
  if (status == WL_CONNECTED) {
    message += String(" ip=") + WiFi.localIP().toString();
    message += String(" rssi=") + String(WiFi.RSSI());
  }
  remoteLog(status == WL_CONNECTED ? "info" : "warn", message);
}

void ensureTimeSyncStarted() {
  if (timeSyncStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configTzTime(TIME_ZONE, NTP_SERVER_1, NTP_SERVER_2);
  timeSyncStarted = true;
  remoteLog("info", String("Time sync requested timezone=") + TIME_ZONE);
}

void logTimeSyncIfReady() {
  if (!timeSyncStarted || timeSyncedLogged) {
    return;
  }

  const uint32_t now = millis();
  if (lastTimeSyncCheckMs != 0 && now - lastTimeSyncCheckMs < 1000) {
    return;
  }
  lastTimeSyncCheckMs = now;

  if (!wallClockReady()) {
    return;
  }

  timeSyncedLogged = true;
  remoteLog("info", String("Time synced ") + formatEpochLocal(time(nullptr)));
}

void logMqttStatusIfChanged() {
  const bool connected = mqtt.connected();
  const int state = mqtt.state();
  if (connected == lastLoggedMqttConnected && state == lastLoggedMqttState) {
    return;
  }
  lastLoggedMqttConnected = connected;
  lastLoggedMqttState = state;
  if (connected) {
    remoteLog("info", "MQTT status connected");
  } else {
    remoteLog("warn", String("MQTT status disconnected state=") + String(state));
  }
}

void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  ensureWifiConnected();
}

void setupRadio() {
  pinMode(RF_TX_PIN, OUTPUT);
  digitalWrite(RF_TX_PIN, LOW);

  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK_PIN, CC1101_MISO_PIN, CC1101_MOSI_PIN, CC1101_CS_PIN);
  ELECHOUSE_cc1101.Init();
  radioReady = ELECHOUSE_cc1101.getCC1101();
  ELECHOUSE_cc1101.setCCMode(0);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(RF_FREQUENCY_MHZ);
  ELECHOUSE_cc1101.setPA(RF_PA_DBM);
  ELECHOUSE_cc1101.setSyncMode(0);
  ELECHOUSE_cc1101.setCrc(false);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.SetTx();

  Serial.printf("CC1101 %s at %.2f MHz\n", radioReady ? "ready" : "not detected", RF_FREQUENCY_MHZ);
  remoteLog(radioReady ? "info" : "error",
            String("CC1101 ") + (radioReady ? "ready" : "not detected") +
              " freq=" + String(RF_FREQUENCY_MHZ, 2));
  if (!radioReady) {
    holdStatusLed(LedMode::Error, 3000);
  }
}

void initBlindStates() {
  for (size_t index = 0; index < BLIND_COUNT; ++index) {
    const BlindDefinition& blind = BLINDS[index];
    blindStates[index] = BlindRuntime{
      &blind,
      blind.initialPosition >= 0 ? clampPosition(blind.initialPosition) : -1,
      blind.initialPosition >= 0 ? clampPosition(blind.initialPosition) : -1,
      blind.initialPosition >= 0 ? clampPosition(blind.initialPosition) : -1,
      0,
      0,
      0,
      Motion::Stopped,
    };
  }
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);

  initStatusLed();
  bootId = esp_random();
  remoteLog("info", String("Boot firmware=") + FIRMWARE_VERSION + " blinds=" + String(BLIND_COUNT));
  configureDebugServer();
  initBlindStates();
  setupRadio();
  setupWifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(handleMqttMessage);
  mqtt.setBufferSize(2048);
}

void loop() {
  updateStatusLed();
  ensureWifiConnected();
  logWifiStatusIfChanged();
  ensureTimeSyncStarted();
  logTimeSyncIfReady();
  ensureDebugServerStarted();
  if (debugServerStarted) {
    debugServer.handleClient();
  }
  ensureOtaStarted();
  if (otaStarted) {
    ArduinoOTA.handle();
  }
  pollRfCapture();
  ensureMqttConnected();
  if (mqtt.connected()) {
    mqtt.loop();
  }
  logMqttStatusIfChanged();
  publishDiagnosticsIfDue();
  updateMotion();
  restartIfRequested();
}

#endif  // RF_CAPTURE_MODE

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>

#include "blinds.h"
#include "config.h"

namespace {

enum class Motion {
  Stopped,
  Opening,
  Closing,
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
BlindRuntime blindStates[BLIND_COUNT];

bool radioReady = false;
uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t lastDiagnosticsPublishMs = 0;

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

bool sendRfCommand(const BlindDefinition& blind, const RawRfCommand& command, String* error = nullptr) {
  if (!radioReady) {
    const String message = "CC1101 radio is not ready";
    if (error != nullptr) {
      *error = message;
    }
    Serial.printf("[%s] %s; cannot send %s\n", blind.id, message.c_str(), command.label);
    return false;
  }
  if (!isRfConfigured(command)) {
    const String message = String("RF command '") + command.label + "' has no pulse data yet";
    if (error != nullptr) {
      *error = message;
    }
    Serial.printf("[%s] %s\n", blind.id, message.c_str());
    return false;
  }

  Serial.printf("[%s] sending RF command '%s' (%u pulses, %u repeats)\n",
                blind.id,
                command.label,
                static_cast<unsigned>(command.pulseCount),
                static_cast<unsigned>(command.repeats));

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
  payload += "\"optimistic\":false,";
  payload += "\"retain\":";
  payload += MQTT_RETAIN ? "true" : "false";
  payload += ",";
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
    return false;
  }

  mqtt.publish(availability.c_str(), "online", true);
  publishAllDiscovery();
  subscribeBlindTopics();
  publishAllStates();
  publishDiagnostics();
  Serial.println("MQTT connected");
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
  WiFi.disconnect(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
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
  ELECHOUSE_cc1101.setCCMode(1);
  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setMHZ(RF_FREQUENCY_MHZ);
  ELECHOUSE_cc1101.setPA(RF_PA_DBM);
  ELECHOUSE_cc1101.setSyncMode(0);
  ELECHOUSE_cc1101.setCrc(false);
  ELECHOUSE_cc1101.setPktFormat(3);
  ELECHOUSE_cc1101.SetTx();

  Serial.printf("CC1101 %s at %.2f MHz\n", radioReady ? "ready" : "not detected", RF_FREQUENCY_MHZ);
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

  initBlindStates();
  setupRadio();
  setupWifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(handleMqttMessage);
  mqtt.setBufferSize(2048);
}

void loop() {
  ensureWifiConnected();
  ensureMqttConnected();
  if (mqtt.connected()) {
    mqtt.loop();
  }
  publishDiagnosticsIfDue();
  updateMotion();
}

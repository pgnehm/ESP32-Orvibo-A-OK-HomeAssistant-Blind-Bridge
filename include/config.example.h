#pragma once

// Copy this file to include/config.local.h and adjust values for your network.

#define WIFI_SSID "CHANGE_ME"
#define WIFI_PASSWORD "CHANGE_ME"

#define MQTT_HOST "homeassistant.local"
#define MQTT_PORT 1883
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define DEVICE_ID "orvibo_esp32_blinds"
#define DEVICE_NAME "Orvibo ESP32 Blind Bridge"
#define FIRMWARE_VERSION "0.2.0"
#define MQTT_BASE_TOPIC "orvibo_esp32_blinds"
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_RETAIN true

// ESP32 VSPI defaults. Keep CC1101 powered from 3.3V only.
#define CC1101_SCK_PIN 18
#define CC1101_MISO_PIN 19
#define CC1101_MOSI_PIN 23
#define CC1101_CS_PIN 5

// Wire this ESP32 GPIO to CC1101 GDO0 for async OOK data input.
#define RF_TX_PIN 27
#define RF_FREQUENCY_MHZ 433.92f
#define RF_PA_DBM 10

#define SERIAL_BAUD 115200
#define WIFI_RECONNECT_INTERVAL_MS 10000
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define POSITION_PUBLISH_INTERVAL_MS 1000
#define DIAGNOSTICS_PUBLISH_INTERVAL_MS 30000

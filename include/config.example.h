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
#define FIRMWARE_VERSION "0.2.11"
#define MQTT_BASE_TOPIC "orvibo_esp32_blinds"
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#define MQTT_RETAIN true

// ArduinoOTA WiFi firmware upload. Empty password means LAN-only but unauthenticated.
#define OTA_PASSWORD ""
#define OTA_PORT 3232

// Used only for readable timestamps in the local web log.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
#define TIME_ZONE "EST5EDT,M3.2.0/2,M11.1.0/2"

// Onboard WS2812 RGB status LED pin. ESP32-S3 N16R8 boards commonly use GPIO48.
// Set to -1 to disable RGB LED output.
#define STATUS_RGB_PIN 48

// ESP32-S3 N16R8 screw-terminal board wiring. Keep CC1101 powered from 3.3V only.
// CC1101 SCK  -> ESP32 GPIO12
// CC1101 MISO -> ESP32 GPIO13
// CC1101 MOSI -> ESP32 GPIO11
// CC1101 CSN  -> ESP32 GPIO10
// CC1101 GDO0 -> ESP32 GPIO14
#define CC1101_SCK_PIN 12
#define CC1101_MISO_PIN 13
#define CC1101_MOSI_PIN 11
#define CC1101_CS_PIN 10

// Wire this ESP32 GPIO to CC1101 GDO0 for async OOK data input/output.
#define RF_TX_PIN 14

// Capture firmware reads raw OOK edges from CC1101 GDO0 on this pin.
// It defaults to the same direct-data pin used for transmit.
#define RF_RX_PIN RF_TX_PIN

#define RF_FREQUENCY_MHZ 433.92f
#define RF_PA_DBM 10

#define SERIAL_BAUD 115200
#define WIFI_RECONNECT_INTERVAL_MS 10000
#define MQTT_RECONNECT_INTERVAL_MS 5000
#define POSITION_PUBLISH_INTERVAL_MS 1000
#define DIAGNOSTICS_PUBLISH_INTERVAL_MS 30000

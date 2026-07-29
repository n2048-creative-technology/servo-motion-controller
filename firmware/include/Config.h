#pragma once

#include <stdint.h>

// arduino-esp32's default C++ standard doesn't reliably provide std::clamp,
// so use a small local helper instead of depending on <algorithm>/C++17.
template <typename T>
static inline T clampValue(T value, T lo, T hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

// ---- Servo ----
// Same silkscreen pin (D10) on both boards, but it's wired to a different
// GPIO: the XIAO ESP32S3 pins D10/D9 are swapped relative to the ESP32C3
// (D10=GPIO9 on the S3 vs D10=GPIO10 on the C3). Selecting by GPIO number
// instead would move the servo signal to a different physical pin per board.
#if defined(ARDUINO_XIAO_ESP32S3)
static constexpr uint8_t SERVO_PIN = 9; // XIAO ESP32S3 D10 / GPIO9
#else
static constexpr uint8_t SERVO_PIN = 10; // XIAO ESP32C3 D10 / GPIO10
#endif
static constexpr uint16_t SERVO_DEFAULT_MIN_US = 500;
static constexpr uint16_t SERVO_DEFAULT_MAX_US = 2500;
static constexpr float SERVO_DEFAULT_MIN_ANGLE = 0.0f;
static constexpr float SERVO_DEFAULT_MAX_ANGLE = 270.0f;
static constexpr float SERVO_DEFAULT_CENTER_ANGLE = 135.0f;

// ---- Timing ----
static constexpr uint32_t TICK_INTERVAL_MS = 20;    // 50 Hz playback tick
static constexpr uint32_t RECORD_INTERVAL_MS = 50;  // 20 Hz recording capture
static constexpr uint32_t STATUS_BROADCAST_MS = 100; // 10 Hz WS status push

// ---- Sequence storage ----
static constexpr uint16_t MAX_SEQ_POINTS = 1200; // 1200 * 50ms = 60s max recording
static constexpr const char *SEQUENCE_FILE_PATH = "/sequence.bin";
static constexpr uint32_t SEQUENCE_FILE_MAGIC = 0x51455331; // "1SEQ"
static constexpr uint16_t SEQUENCE_FILE_VERSION = 1;

// ---- Settings / NVS ----
static constexpr const char *NVS_NAMESPACE = "app";
static constexpr const char *NVS_KEY = "cfg";
static constexpr uint32_t SETTINGS_MAGIC = 0x53565831; // "1XVS"
static constexpr uint16_t SETTINGS_VERSION = 3; // v3: added servoInvert (older blobs fall back to defaults)

// ---- WiFi AP defaults ----
static constexpr const char *AP_SSID_PREFIX = "ServoRig-";
static constexpr const char *AP_DEFAULT_PASSWORD = "servo1234";
static constexpr uint8_t AP_WIFI_CHANNEL = 1;

// ---- Web server ----
static constexpr uint16_t HTTP_PORT = 80;
static constexpr uint16_t DNS_PORT = 53;

// ---- Firmware version ----
static constexpr const char *FIRMWARE_VERSION = "2.0.0";

// ---- Master/Node network (ESP-NOW) ----
static constexpr uint8_t NET_PACKET_MAGIC = 0xE5;
static constexpr uint8_t NET_PACKET_VERSION = 2; // v2: added sessionId/seq (stale/out-of-order CMD rejection)
static constexpr uint8_t NET_BROADCAST_NODE = 0; // targetNode value meaning "all nodes"
static constexpr uint8_t NET_NODE_ID_MIN = 1;
static constexpr uint8_t NET_NODE_ID_MAX = 250;
static constexpr uint32_t NET_HELLO_INTERVAL_MS = 1000;  // node -> master heartbeat rate
static constexpr uint32_t NET_NODE_STALE_MS = 5000;      // drop from master's table if unseen this long
static constexpr uint8_t NET_MAX_TRACKED_NODES = 32;     // master's known-node table size

// Master: re-broadcasts the last angle sent to each target at least this
// often, regardless of whether it actually changed. Self-heals a target that
// missed its most recent CMD (e.g. dropped during radio contention from a
// phone joining a Node's AP) without needing a reboot or a new movement to
// trigger a fresh send.
static constexpr uint32_t NET_CMD_RESEND_INTERVAL_MS = 300;
static constexpr uint8_t NET_MAX_LAST_COMMANDS = 16; // distinct targets (node ids or broadcast) tracked for resend

// Node: periodically re-applies the last commanded angle to the servo, so a
// missed update is corrected as soon as ESP-NOW recovers rather than the
// board sitting at a stale position indefinitely.
static constexpr uint32_t SERVO_REAPPLY_INTERVAL_MS = 250;

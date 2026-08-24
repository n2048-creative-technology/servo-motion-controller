#pragma once

#include <stddef.h>
#include <stdint.h>

// arduino-esp32's default C++ standard doesn't reliably provide std::clamp,
// so use a small local helper instead of depending on <algorithm>/C++17.
template <typename T>
static inline T clampValue(T value, T lo, T hi) {
  return value < lo ? lo : (value > hi ? hi : value);
}

// ---- Servos (pan/tilt) ----
// A node drives a two-axis head: X (pan) on D10, Y (tilt) on D3.
//
// Both are picked by silkscreen pin rather than GPIO number, because the same
// label sits on a different GPIO per board: the XIAO ESP32S3's D10/D9 are
// swapped relative to the ESP32C3 (D10=GPIO9 on the S3 vs D10=GPIO10 on the
// C3). Selecting by GPIO number instead would move a servo signal to a
// different physical pin depending on which board is being built.
//
// D3 is GPIO5 on the C3 and GPIO4 on the S3 — neither is a strapping pin on
// its chip, and neither is used by anything else in this firmware.
//
// Both servos share LEDC timer 0: they run at the same 50Hz, which is exactly
// what a shared timer supports, and the C3 (the tighter of the two chips) has
// 6 LEDC channels for 2 servos. See ServoController::begin().
#if defined(ARDUINO_XIAO_ESP32S3)
static constexpr uint8_t SERVO_X_PIN = 9; // XIAO ESP32S3 D10 / GPIO9
static constexpr uint8_t SERVO_Y_PIN = 4; // XIAO ESP32S3 D3  / GPIO4
#else
static constexpr uint8_t SERVO_X_PIN = 10; // XIAO ESP32C3 D10 / GPIO10
static constexpr uint8_t SERVO_Y_PIN = 5;  // XIAO ESP32C3 D3  / GPIO5
#endif
static constexpr uint16_t SERVO_DEFAULT_MIN_US = 500;
static constexpr uint16_t SERVO_DEFAULT_MAX_US = 2500;
static constexpr float SERVO_DEFAULT_MIN_ANGLE = 0.0f;
static constexpr float SERVO_DEFAULT_MAX_ANGLE = 270.0f;
static constexpr float SERVO_DEFAULT_CENTER_ANGLE = 135.0f;

// ---- Relay (light switch) ----
// Same silkscreen pin (D7) on both boards but a different GPIO underneath,
// for the same reason SERVO_PIN above is selected per board rather than by
// GPIO number. D7 doubles as UART0's RX pin on both XIAOs; it's free to use
// as an ordinary output here because both board definitions build with
// ARDUINO_USB_CDC_ON_BOOT=1, so Serial — including the Master's PC bridge —
// runs over native USB and never touches UART0. See docs/wiring.md.
#if defined(ARDUINO_XIAO_ESP32S3)
static constexpr uint8_t RELAY_PIN = 44; // XIAO ESP32S3 D7 / GPIO44
#else
static constexpr uint8_t RELAY_PIN = 20; // XIAO ESP32C3 D7 / GPIO20
#endif
// Most opto-isolated relay boards close the contact when their IN pin is
// pulled *low*; bare transistor/MOSFET drivers are the other way around.
// Configurable in Settings so either wiring works without a reflash.
static constexpr bool RELAY_DEFAULT_ACTIVE_LOW = false;

// ---- RANDOM pattern ----
// Dwell between moves is drawn uniformly from [min, max]; each move itself is
// slew-limited to maxSpeed and eased in/out, so the servo is never asked for
// the instant step a naive "jump to a random angle" would produce. See
// PatternEngine::computeRandomAngle().
static constexpr uint32_t PATTERN_RANDOM_DEFAULT_MIN_INTERVAL_MS = 1500;
static constexpr uint32_t PATTERN_RANDOM_DEFAULT_MAX_INTERVAL_MS = 6000;
static constexpr float PATTERN_RANDOM_DEFAULT_MAX_SPEED_DPS = 90.0f;
static constexpr float PATTERN_RANDOM_MIN_SPEED_DPS = 5.0f;
// A 270-degree hobby servo's own no-load speed is roughly 300-600 deg/s, so
// anything above this is asking for more than the gear train can deliver —
// the servo would just saturate and the motion stops being predictable.
static constexpr float PATTERN_RANDOM_MAX_SPEED_DPS = 400.0f;
static constexpr uint32_t PATTERN_RANDOM_MIN_MOVE_MS = 150;  // floor for very short hops
static constexpr uint32_t PATTERN_RANDOM_MIN_SETTLE_MS = 100; // guaranteed hold after arriving

// ---- Timing ----
static constexpr uint32_t TICK_INTERVAL_MS = 20;    // 50 Hz playback tick
static constexpr uint32_t RECORD_INTERVAL_MS = 50;  // 20 Hz recording capture
static constexpr uint32_t STATUS_BROADCAST_MS = 100; // 10 Hz WS status push

// ---- Sequence storage ----
// 8000 * 50ms = 400s (6min40s) max recording. This is a fixed-size static
// array (SequenceStore::points_, 12 bytes/point), not heap — it's carved out
// of every board's RAM at link time whether or not it's ever filled, so
// changing it is a firmware-wide RAM budget decision shared by every board
// (Master and Node roles both link SequenceStore in). Sized to leave a
// healthy safety margin against the tightest board in this project's
// current lineup, the XIAO ESP32C3, which had ~107KB free heap for
// WiFi/AsyncWebServer/ESP-NOW/JSON with the previous 96KB array.
//
// When each point gained a Y axis it grew 8 -> 12 bytes, so the count came
// down 12000 -> 8000 to hold the array at that same 96KB rather than spend
// another 48KB of a budget that was already the tightest thing on this
// board. The cost is max recording length: 10min -> 6min40s. See
// docs/serial-protocol.md.
static constexpr uint16_t MAX_SEQ_POINTS = 8000;
static constexpr const char *SEQUENCE_LEGACY_FILE_PATH = "/sequence.bin"; // v1 single fixed file, migrated once
static constexpr uint32_t SEQUENCE_FILE_MAGIC = 0x51455331; // "1SEQ"
// v2: each point carries the relay/light state alongside the angle (the flags
// byte landed in what v1 left as padding, so v1 and v2 records are both 8
// bytes). v3: a second servo axis, so a point is {t, x, y, flags} = 12 bytes.
// v1/v2 files still load — their 8-byte records are read and expanded, and
// the sequence is marked as having no Y track so playback leaves that axis
// alone rather than inventing a position for it. See
// SequenceStore::loadNamed().
static constexpr uint16_t SEQUENCE_FILE_VERSION = 3;
static constexpr uint16_t SEQUENCE_FILE_VERSION_MIN = 1; // oldest still readable
static constexpr uint16_t SEQUENCE_FILE_VERSION_XY = 3;  // first version with a Y axis
static constexpr size_t SEQUENCE_POINT_BYTES_V1 = 8;     // {t, angle, flags, pad}
static constexpr const char *SEQUENCE_DIR = "/seq"; // v2+: one "<name>.bin" file per named sequence
static constexpr uint8_t SEQ_NAME_MAX_LEN = 23;      // + null terminator = sizeof(NetPacket::seqName)
static constexpr uint8_t SEQ_MAX_LISTED = 16;        // cap for directory listing / UI dropdowns
// Ceiling on how many points GET /api/sequence/data returns. The Record tab's
// plot canvas is 600px wide, so a few hundred samples already draw every
// feature a longer recording has — sending thousands would cost heap and
// airtime for pixels that don't exist.
static constexpr uint16_t SEQ_PLOT_MAX_POINTS = 300;
static constexpr const char *SEQUENCE_LEGACY_MIGRATED_NAME = "local";

// ---- Settings / NVS ----
static constexpr const char *NVS_NAMESPACE = "app";
static constexpr const char *NVS_KEY = "cfg";
static constexpr uint32_t SETTINGS_MAGIC = 0x53565831; // "1XVS"
static constexpr uint16_t SETTINGS_VERSION = 6; // v6: per-axis (X/Y) servo calibration + per-axis autostart pattern (older blobs fall back to defaults)

// Network identity (SSID, password, network mode, node id) lives under its
// own NVS key, deliberately separate from NVS_KEY/SETTINGS_VERSION above —
// see SettingsStore::loadNetworkIdentity()'s comment. A board's WiFi
// name/password and its Master/Node role shouldn't reset just because some
// unrelated settings field (a pattern parameter, a servo calibration value)
// changed shape and bumped SETTINGS_VERSION.
static constexpr const char *NET_IDENTITY_NVS_KEY = "netid";
static constexpr uint32_t NET_IDENTITY_MAGIC = 0x4E455431; // "NET1"

// ---- WiFi AP defaults ----
static constexpr const char *AP_SSID_PREFIX = "ServoRig-";
static constexpr const char *AP_DEFAULT_PASSWORD = "servo1234";
static constexpr uint8_t AP_WIFI_CHANNEL = 1;

// A Node drops its WiFi (AP + ESP-NOW, same radio) TX power while its servo
// is actively being driven (PlaybackEngine::mode() != IDLE) and restores it
// to full power once idle — WiFi TX draws its own current spike on top of
// whatever the servo itself pulls, and shaving that off during exactly the
// window a high-motion recording can already brown a Node out (see
// docs/serial-protocol.md) trims the combined peak. A Master never carries
// this cut — it drives no servo of its own, so there's no local current
// spike to protect against, and it needs its own best possible range/
// reliability for every Node's HELLO/CMD/SEQ_ACK traffic — so it's held at
// WIFI_TX_POWER_MAX always (see main.cpp's setup()). Values are raw
// wifi_power_t enum values from WiFiGeneric.h (kept as plain ints here so
// Config.h doesn't need to pull in WiFi.h); cast at the WiFi.setTxPower()
// call site. "Reduced" is WIFI_POWER_13dBm — ~20% of WIFI_POWER_19_5dBm's
// max in actual linear RF power (dBm is logarithmic, so 20% of the *number*
// would be a much bigger cut than 20% of the power itself), the closest
// step this API offers to that. Lower TX power also means shorter
// AP/ESP-NOW range while a Node is moving — expect that trade-off.
static constexpr uint8_t WIFI_TX_POWER_MAX = 78;         // WIFI_POWER_19_5dBm (default/max)
static constexpr uint8_t NODE_WIFI_TX_POWER_ACTIVE = 52; // WIFI_POWER_13dBm (~20% of max linear power)

// ---- Web server ----
static constexpr uint16_t HTTP_PORT = 80;
static constexpr uint16_t DNS_PORT = 53;

// ---- Firmware version ----
// Bump this whenever a release changes what the API/UI can do — it's the
// only way to tell from the outside which build a board is actually running
// (GET /api/status, and the Settings tab shows it). 2.1.0: RANDOM pattern,
// relay/light output on D7, servo-range-aware jog fader. 2.2.0: second servo
// axis (pan/tilt) on D3, XY trackpad, per-axis patterns.
static constexpr const char *FIRMWARE_VERSION = "2.2.0";

// ---- Master/Node network (ESP-NOW) ----
static constexpr uint8_t NET_PACKET_MAGIC = 0xE5;
static constexpr uint8_t NET_PACKET_VERSION = 6; // v6: CMD/HELLO carry both servo axes (X and Y)
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

// Master: how many SEQ_ACKs can be queued between one loopTick() and the
// next before an overflowing one is dropped. Needs to be more than 1 now
// that "upload to all Nodes" runs several Nodes' uploads concurrently —
// their SEQ_ACKs can arrive close enough together that a single pending
// slot would let a later one silently overwrite an earlier, still-unread
// one (see NetworkLink.h's pendingAcks_ comment).
static constexpr uint8_t NET_MAX_PENDING_ACKS = 8;

// Node: periodically re-applies the last commanded angle to the servo, so a
// missed update is corrected as soon as ESP-NOW recovers rather than the
// board sitting at a stale position indefinitely.
static constexpr uint32_t SERVO_REAPPLY_INTERVAL_MS = 250;

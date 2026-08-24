#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Config.h"
#include "PatternEngine.h"

enum class AutostartTarget : uint8_t {
  NONE = 0,
  PATTERN = 1,
  SEQUENCE = 2
};

// STANDALONE: today's behavior, no ESP-NOW. NODE: joins a Master's network,
// accepts wireless positioning commands in addition to local jog/pattern/
// sequence control. MASTER: bridges USB-serial commands from a PC to CMD
// broadcasts for NODEs; no local servo/playback role.
enum class OperatingMode : uint8_t {
  STANDALONE = 0,
  NODE = 1,
  MASTER = 2
};

// One servo axis's calibration, stored twice in PersistedSettings.
struct ServoCalibration {
  uint16_t minUs = SERVO_DEFAULT_MIN_US;
  uint16_t maxUs = SERVO_DEFAULT_MAX_US;
  float minAngle = SERVO_DEFAULT_MIN_ANGLE;
  float maxAngle = SERVO_DEFAULT_MAX_ANGLE;
  float centerAngle = SERVO_DEFAULT_CENTER_ANGLE;
  bool invert = false;
};

struct PersistedSettings {
  uint32_t magic = SETTINGS_MAGIC;
  uint16_t version = SETTINGS_VERSION;

  char apSsid[33] = {0};
  char apPassword[65] = {0};

  // One block per axis: X (pan, D10) and Y (tilt, D3) are usually different
  // servos with different mechanical ranges, so they calibrate separately.
  ServoCalibration servoX;
  ServoCalibration servoY;

  // Relay/light output on D7 — see RELAY_PIN in Config.h.
  bool relayActiveLow = RELAY_DEFAULT_ACTIVE_LOW;

  bool autostartEnabled = false;
  AutostartTarget autostartTarget = AutostartTarget::NONE;
  // One pattern per axis: the two run independently, so a head can (say) sweep
  // in X while holding a slow triangle in Y.
  PatternParams autostartPatternX;
  PatternParams autostartPatternY;
  char autostartSequenceName[24] = {0}; // which /seq/<name>.bin to loop, when target == SEQUENCE

  OperatingMode networkMode = OperatingMode::STANDALONE;
  uint8_t nodeId = NET_NODE_ID_MIN;
};

class SettingsStore {
public:
  // Loads from NVS, or writes+applies factory defaults if absent/corrupt/version-mismatched.
  void load();
  void save();
  void factoryDefaults();

  PersistedSettings &settings() { return settings_; }
  const PersistedSettings &settings() const { return settings_; }

private:
  PersistedSettings settings_;
  void generateDefaultSsid(char *out, size_t outLen);
  // Overlays settings_'s SSID/password/network mode/node id from their own
  // NVS key, or seeds that key from settings_'s current values if it doesn't
  // exist yet. See Config.h's NET_IDENTITY_NVS_KEY comment.
  void loadNetworkIdentity();
  void saveNetworkIdentity();
};

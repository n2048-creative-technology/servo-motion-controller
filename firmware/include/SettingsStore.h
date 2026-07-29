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

struct PersistedSettings {
  uint32_t magic = SETTINGS_MAGIC;
  uint16_t version = SETTINGS_VERSION;

  char apSsid[33] = {0};
  char apPassword[65] = {0};

  uint16_t servoMinUs = SERVO_DEFAULT_MIN_US;
  uint16_t servoMaxUs = SERVO_DEFAULT_MAX_US;
  float servoMinAngle = SERVO_DEFAULT_MIN_ANGLE;
  float servoMaxAngle = SERVO_DEFAULT_MAX_ANGLE;
  float servoCenterAngle = SERVO_DEFAULT_CENTER_ANGLE;
  bool servoInvert = false;

  bool autostartEnabled = false;
  AutostartTarget autostartTarget = AutostartTarget::NONE;
  PatternParams autostartPattern;

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
};

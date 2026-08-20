#pragma once

#include <stdint.h>
#include "PatternEngine.h"
#include "SettingsStore.h"
#include "IAngleSink.h"
#include "IRelaySink.h"

class SequenceStore;

enum class PlaybackMode : uint8_t {
  IDLE = 0,
  MANUAL = 1,
  RECORDING = 2,
  PATTERN = 3,
  SEQUENCE = 4,
  NETWORK = 5  // driven by a wireless CMD from a Master, not the local jog slider
};

// Decides "what angle should the servo be at right now" for every mode and
// drives the ServoController accordingly. All entry points take an explicit
// `now` (millis()) so behavior is deterministic and callable before WiFi/web
// server init (autostart) as well as from REST/WS handlers at request time.
class PlaybackEngine {
public:
  void begin(IAngleSink *sink, SequenceStore *sequence, IRelaySink *relay);

  // The servo's calibrated travel, as configured in Settings. Used by the
  // RANDOM pattern to pick targets that stay inside the servo's own limits;
  // WebApi re-applies it whenever calibration changes.
  void setAngleLimits(float minDeg, float maxDeg);

  // Call every TICK_INTERVAL_MS from loop().
  void tick(uint32_t now);

  // Immediate manual move; interrupts PATTERN/SEQUENCE playback but not RECORDING
  // (during recording, jogging is what drives the motion being captured).
  void onJog(float angleDeg, uint32_t now);

  // Immediate move driven by a wireless CMD from a Master (NODE mode only).
  // Same interrupt semantics as onJog, but tagged as PlaybackMode::NETWORK so
  // status/UI can distinguish "moved by the network" from local jogging. The
  // relay state rides along on the same command.
  void onNetworkCommand(float angleDeg, bool relayOn, uint32_t now);

  // Manual relay/light toggle. Unlike a jog it doesn't change mode: switching
  // the light shouldn't stop a running pattern, and during RECORDING it's
  // captured by the ordinary capture tick just like the angle is.
  void onRelayToggle(bool on);
  bool relayState() const { return relay_ ? relay_->relayState() : false; }

  void startPattern(const PatternParams &params, uint32_t now);
  void stopPattern();

  void startRecording(uint32_t now);
  void stopRecording();

  void startSequencePlayback(uint32_t now);
  void stopSequencePlayback();

  // Called once in setup(), before WiFi/web server init, so a configured
  // pattern or sequence is already looping with zero user interaction.
  void applyAutostart(const PersistedSettings &settings, uint32_t now);

  PlaybackMode mode() const { return mode_; }
  const PatternParams &activePattern() const { return activePattern_; }

private:
  IAngleSink *servo_ = nullptr;
  SequenceStore *sequence_ = nullptr;
  IRelaySink *relay_ = nullptr;
  PlaybackMode mode_ = PlaybackMode::IDLE;
  uint32_t modeStartMs_ = 0;
  uint32_t lastRecordCaptureMs_ = 0;
  uint32_t lastReapplyMs_ = 0;
  PatternParams activePattern_;
  RandomPatternState randomState_;
  float minAngleDeg_ = SERVO_DEFAULT_MIN_ANGLE;
  float maxAngleDeg_ = SERVO_DEFAULT_MAX_ANGLE;
};

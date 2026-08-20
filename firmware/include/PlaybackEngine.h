#pragma once

#include <stdint.h>
#include "PatternEngine.h"
#include "SettingsStore.h"
#include "IMotionSink.h"
#include "IRelaySink.h"

class SequenceStore;

enum class PlaybackMode : uint8_t {
  IDLE = 0,
  MANUAL = 1,
  RECORDING = 2,
  PATTERN = 3,
  SEQUENCE = 4,
  NETWORK = 5  // driven by a wireless CMD from a Master, not the local trackpad
};

// Decides "where should the head be pointing right now" for every mode and
// drives the motion sink accordingly. All entry points take an explicit `now`
// (millis()) so behavior is deterministic and callable before WiFi/web server
// init (autostart) as well as from REST/WS handlers at request time.
//
// Both servo axes are always driven together in one sink call — see
// IMotionSink for why that matters on a Master.
class PlaybackEngine {
public:
  void begin(IMotionSink *sink, SequenceStore *sequence, IRelaySink *relay);

  // Each axis's calibrated travel, as configured in Settings. Used by the
  // RANDOM pattern to pick targets that stay inside the servos' own limits;
  // WebApi re-applies this whenever calibration changes.
  void setAngleLimits(float minX, float maxX, float minY, float maxY);

  // Call every TICK_INTERVAL_MS from loop().
  void tick(uint32_t now);

  // Immediate manual move; interrupts PATTERN/SEQUENCE playback but not
  // RECORDING (during recording, jogging is what drives the motion being
  // captured).
  void onJog(float xDeg, float yDeg, uint32_t now);

  // Immediate move driven by a wireless CMD from a Master (NODE mode only).
  // Same interrupt semantics as onJog, but tagged as PlaybackMode::NETWORK so
  // status/UI can distinguish "moved by the network" from local jogging. The
  // relay state rides along on the same command.
  void onNetworkCommand(float xDeg, float yDeg, bool relayOn, uint32_t now);

  // Manual relay/light toggle. Unlike a jog it doesn't change mode: switching
  // the light shouldn't stop a running pattern, and during RECORDING it's
  // captured by the ordinary capture tick just like the angles are.
  void onRelayToggle(bool on);
  bool relayState() const { return relay_ ? relay_->relayState() : false; }

  // One pattern per axis; either may be a different shape, and either may be
  // RANDOM independently of the other.
  void startPattern(const PatternParams &paramsX, const PatternParams &paramsY, uint32_t now);
  void stopPattern();

  void startRecording(uint32_t now);
  void stopRecording();

  void startSequencePlayback(uint32_t now);
  void stopSequencePlayback();

  // Called once in setup(), before WiFi/web server init, so a configured
  // pattern or sequence is already looping with zero user interaction.
  void applyAutostart(const PersistedSettings &settings, uint32_t now);

  PlaybackMode mode() const { return mode_; }
  const PatternParams &activePatternX() const { return patternX_; }
  const PatternParams &activePatternY() const { return patternY_; }

private:
  // True when both axes are RANDOM, in which case they share one schedule —
  // see PatternEngine::computeRandom2D.
  bool randomLinked() const {
    return patternX_.type == PatternType::RANDOM && patternY_.type == PatternType::RANDOM;
  }
  void computePattern(uint32_t elapsed, float *outX, float *outY);

  IMotionSink *servos_ = nullptr;
  SequenceStore *sequence_ = nullptr;
  IRelaySink *relay_ = nullptr;
  PlaybackMode mode_ = PlaybackMode::IDLE;
  uint32_t modeStartMs_ = 0;
  uint32_t lastRecordCaptureMs_ = 0;
  uint32_t lastReapplyMs_ = 0;
  PatternParams patternX_;
  PatternParams patternY_;
  Random2DState randomState_;
  float minXDeg_ = SERVO_DEFAULT_MIN_ANGLE;
  float maxXDeg_ = SERVO_DEFAULT_MAX_ANGLE;
  float minYDeg_ = SERVO_DEFAULT_MIN_ANGLE;
  float maxYDeg_ = SERVO_DEFAULT_MAX_ANGLE;
};

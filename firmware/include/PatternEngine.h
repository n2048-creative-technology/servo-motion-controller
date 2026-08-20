#pragma once

#include <stdint.h>
#include "Config.h"

enum class PatternType : uint8_t {
  SINE = 0,
  SQUARE = 1,
  TRIANGLE = 2,
  SAWTOOTH = 3,
  TRAPEZOID = 4,
  RANDOM = 5
};

// Angle produced by a pattern is offsetDeg + amplitudeDeg * shape(phase),
// where shape(phase) is a normalized waveform in [-1, 1].
//
// RANDOM is the exception: it ignores period/amplitude/offset entirely and is
// driven by the three rand* fields below plus the servo's own calibrated angle
// range (see PatternEngine::computeRandomAngle).
struct PatternParams {
  PatternType type = PatternType::SINE;
  uint32_t periodMs = 2000;
  float amplitudeDeg = 45.0f;
  float offsetDeg = 135.0f;
  float dutyPct = 50.0f;   // SQUARE: percent of period spent high
  float risePct = 25.0f;   // TRAPEZOID: percent of period ramping up
  float holdPct = 25.0f;   // TRAPEZOID: percent of period held high
  float fallPct = 25.0f;   // TRAPEZOID: percent of period ramping down (remainder is held low)

  // RANDOM only. Each cycle waits a duration drawn uniformly from
  // [randMinIntervalMs, randMaxIntervalMs] before starting the next move; the
  // move itself never exceeds randMaxSpeedDps.
  uint32_t randMinIntervalMs = PATTERN_RANDOM_DEFAULT_MIN_INTERVAL_MS;
  uint32_t randMaxIntervalMs = PATTERN_RANDOM_DEFAULT_MAX_INTERVAL_MS;
  float randMaxSpeedDps = PATTERN_RANDOM_DEFAULT_MAX_SPEED_DPS;
};

// RANDOM's per-run state. Unlike every other shape — a pure function of
// (params, phase) — RANDOM has to remember where the current move started,
// where it's heading and when the next one is due, so PlaybackEngine owns one
// of these and resets it each time a RANDOM pattern starts.
struct RandomPatternState {
  bool initialized = false;
  float fromDeg = 0.0f;      // where the current move started
  float toDeg = 0.0f;        // where it's heading
  uint32_t moveStartMs = 0;  // elapsed-time stamp the current move began at
  uint32_t moveDurationMs = 0;
  uint32_t nextMoveMs = 0;   // elapsed-time stamp the next move begins at
};

namespace PatternEngine {

// t_ms is elapsed time since the pattern started (wraps internally by period).
// RANDOM is not handled here (it needs state) — it returns offsetDeg, so a
// caller that forgets to route RANDOM to computeRandomAngle() below holds
// still rather than doing something unpredictable with the servo.
float computeAngle(const PatternParams &params, uint32_t t_ms);

// RANDOM: the angle at elapsed time t_ms, advancing `state` as needed.
// [minDeg, maxDeg] is the servo's calibrated travel — every target is drawn
// from inside it, so a random pattern can never command the servo past its
// configured limits.
//
// Motion between two targets is eased (smoothstep: zero velocity at both
// ends) and stretched to whatever duration keeps peak speed at or below
// params.randMaxSpeedDps, so what reaches the servo is a ramp it can actually
// track — never the instantaneous jump that makes a servo slam its gears and
// brown out its supply.
float computeRandomAngle(const PatternParams &params, RandomPatternState &state, uint32_t t_ms,
                         float minDeg, float maxDeg);

// Puts `state` back to "about to make its first move", starting from wherever
// the servo currently sits so the opening move is as smooth as every later one.
void resetRandom(RandomPatternState &state, float currentAngleDeg);

// Returns false and clamps unknown types to a safe default if invalid.
bool isValidType(uint8_t rawType);

} // namespace PatternEngine

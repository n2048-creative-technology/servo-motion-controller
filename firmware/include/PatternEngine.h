#pragma once

#include <stdint.h>

enum class PatternType : uint8_t {
  SINE = 0,
  SQUARE = 1,
  TRIANGLE = 2,
  SAWTOOTH = 3,
  TRAPEZOID = 4
};

// Angle produced by a pattern is offsetDeg + amplitudeDeg * shape(phase),
// where shape(phase) is a normalized waveform in [-1, 1].
struct PatternParams {
  PatternType type = PatternType::SINE;
  uint32_t periodMs = 2000;
  float amplitudeDeg = 45.0f;
  float offsetDeg = 135.0f;
  float dutyPct = 50.0f;   // SQUARE: percent of period spent high
  float risePct = 25.0f;   // TRAPEZOID: percent of period ramping up
  float holdPct = 25.0f;   // TRAPEZOID: percent of period held high
  float fallPct = 25.0f;   // TRAPEZOID: percent of period ramping down (remainder is held low)
};

namespace PatternEngine {

// t_ms is elapsed time since the pattern started (wraps internally by period).
float computeAngle(const PatternParams &params, uint32_t t_ms);

// Returns false and clamps unknown types to a safe default if invalid.
bool isValidType(uint8_t rawType);

} // namespace PatternEngine

#include "PatternEngine.h"
#include "Config.h"

#include <math.h>

namespace {

constexpr float TWO_PI_F = 6.28318530718f;

float shapeSine(float phase) { return sinf(TWO_PI_F * phase); }

float shapeSquare(float phase, float dutyPct) {
  const float duty = clampValue(dutyPct, 0.0f, 100.0f) / 100.0f;
  return phase < duty ? 1.0f : -1.0f;
}

float shapeTriangle(float phase) {
  // 0 -> 1 over first half, 1 -> -1... actually symmetric: -1 at 0, +1 at 0.5, -1 at 1
  if (phase < 0.5f) {
    return -1.0f + 4.0f * phase;
  }
  return 3.0f - 4.0f * phase;
}

float shapeSawtooth(float phase) {
  return -1.0f + 2.0f * phase;
}

float shapeTrapezoid(float phase, float risePct, float holdPct, float fallPct) {
  float rise = clampValue(risePct, 0.0f, 100.0f) / 100.0f;
  float hold = clampValue(holdPct, 0.0f, 100.0f) / 100.0f;
  float fall = clampValue(fallPct, 0.0f, 100.0f) / 100.0f;
  const float total = rise + hold + fall;
  if (total > 1.0f && total > 0.0f) {
    // Normalize so the three segments never exceed the full period.
    rise /= total;
    hold /= total;
    fall /= total;
  }
  const float lowHoldStart = rise + hold + fall;

  if (phase < rise) {
    return rise > 0.0f ? (-1.0f + 2.0f * (phase / rise)) : 1.0f;
  }
  if (phase < rise + hold) {
    return 1.0f;
  }
  if (phase < lowHoldStart) {
    return fall > 0.0f ? (1.0f - 2.0f * ((phase - rise - hold) / fall)) : -1.0f;
  }
  return -1.0f;
}

} // namespace

namespace PatternEngine {

float computeAngle(const PatternParams &params, uint32_t t_ms) {
  const uint32_t period = params.periodMs > 0 ? params.periodMs : 1;
  const float phase = static_cast<float>(t_ms % period) / static_cast<float>(period);

  float shape;
  switch (params.type) {
    case PatternType::SINE:
      shape = shapeSine(phase);
      break;
    case PatternType::SQUARE:
      shape = shapeSquare(phase, params.dutyPct);
      break;
    case PatternType::TRIANGLE:
      shape = shapeTriangle(phase);
      break;
    case PatternType::SAWTOOTH:
      shape = shapeSawtooth(phase);
      break;
    case PatternType::TRAPEZOID:
      shape = shapeTrapezoid(phase, params.risePct, params.holdPct, params.fallPct);
      break;
    default:
      shape = 0.0f;
      break;
  }

  return params.offsetDeg + params.amplitudeDeg * shape;
}

bool isValidType(uint8_t rawType) {
  return rawType <= static_cast<uint8_t>(PatternType::TRAPEZOID);
}

} // namespace PatternEngine

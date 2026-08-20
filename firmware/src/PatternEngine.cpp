#include "PatternEngine.h"
#include "Config.h"

#include <esp_random.h>
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

// Uniform in [lo, hi]. esp_random() is the hardware RNG, so this doesn't
// share (or need seeding alongside) Arduino's random() sequence.
float randomInRange(float lo, float hi) {
  if (hi <= lo) return lo;
  const float unit = static_cast<float>(esp_random()) / 4294967296.0f; // [0, 1)
  return lo + unit * (hi - lo);
}

uint32_t randomIntervalMs(uint32_t lo, uint32_t hi) {
  if (hi <= lo) return lo;
  return lo + (esp_random() % (hi - lo + 1));
}

// Smoothstep: 0 at u=0, 1 at u=1, zero slope at both ends — the servo leaves
// and reaches each target gradually instead of stepping.
float smoothstep(float u) { return u * u * (3.0f - 2.0f * u); }

// Picks the next target and how long moving there should take, then schedules
// the move after it. Called once per cycle from computeRandomAngle().
void planNextRandomMove(const PatternParams &params, RandomPatternState &state, float minDeg, float maxDeg) {
  state.fromDeg = state.toDeg;
  state.toDeg = randomInRange(minDeg, maxDeg);

  const float speed = clampValue(params.randMaxSpeedDps, PATTERN_RANDOM_MIN_SPEED_DPS, PATTERN_RANDOM_MAX_SPEED_DPS);
  const float distance = fabsf(state.toDeg - state.fromDeg);
  // Smoothstep's peak velocity is 1.5x its average, so stretch the move by
  // that factor: what the user sets as "max speed" is the actual peak the
  // servo sees mid-move, not an average it overshoots.
  // Rounded up, not truncated: shaving the fractional millisecond off would
  // make the move finish fractionally early, i.e. fractionally faster than
  // the speed cap the user set.
  uint32_t duration = static_cast<uint32_t>(ceilf((1.5f * distance / speed) * 1000.0f));
  if (duration < PATTERN_RANDOM_MIN_MOVE_MS) duration = PATTERN_RANDOM_MIN_MOVE_MS;

  uint32_t lo = params.randMinIntervalMs;
  uint32_t hi = params.randMaxIntervalMs;
  if (hi < lo) {  // user entered them backwards — treat as an unordered pair
    const uint32_t swap = lo;
    lo = hi;
    hi = swap;
  }
  uint32_t interval = randomIntervalMs(lo, hi);
  // The interval covers the whole cycle (move + hold). A long move under a
  // short interval would otherwise be cut off mid-travel by the next target,
  // which is exactly the discontinuity the easing exists to avoid.
  const uint32_t minCycle = duration + PATTERN_RANDOM_MIN_SETTLE_MS;
  if (interval < minCycle) interval = minCycle;

  state.moveStartMs = state.nextMoveMs;
  state.moveDurationMs = duration;
  state.nextMoveMs = state.moveStartMs + interval;
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
    case PatternType::RANDOM:
      // Stateful — see computeRandomAngle(). Hold at the offset here rather
      // than picking some arbitrary shape.
      return params.offsetDeg;
    default:
      shape = 0.0f;
      break;
  }

  return params.offsetDeg + params.amplitudeDeg * shape;
}

void resetRandom(RandomPatternState &state, float currentAngleDeg) {
  state = RandomPatternState{};
  state.fromDeg = currentAngleDeg;
  state.toDeg = currentAngleDeg;
}

float computeRandomAngle(const PatternParams &params, RandomPatternState &state, uint32_t t_ms,
                         float minDeg, float maxDeg) {
  if (maxDeg < minDeg) {
    const float swap = minDeg;
    minDeg = maxDeg;
    maxDeg = swap;
  }

  if (!state.initialized) {
    state.initialized = true;
    state.fromDeg = clampValue(state.fromDeg, minDeg, maxDeg);
    state.toDeg = state.fromDeg;
    state.moveStartMs = t_ms;
    state.moveDurationMs = 0;
    state.nextMoveMs = t_ms; // first move is planned on this very tick
  }

  // Signed comparisons so a pattern left running past millis()' ~49-day wrap
  // keeps scheduling instead of stalling. The loop (rather than an if) also
  // catches up correctly if a tick is ever delayed past a whole short cycle.
  while (static_cast<int32_t>(t_ms - state.nextMoveMs) >= 0) {
    planNextRandomMove(params, state, minDeg, maxDeg);
  }

  const uint32_t elapsedInMove = t_ms - state.moveStartMs;
  if (state.moveDurationMs == 0 || elapsedInMove >= state.moveDurationMs) {
    return state.toDeg; // arrived: hold here until the next move is due
  }
  const float u = static_cast<float>(elapsedInMove) / static_cast<float>(state.moveDurationMs);
  return state.fromDeg + (state.toDeg - state.fromDeg) * smoothstep(u);
}

bool isValidType(uint8_t rawType) {
  return rawType <= static_cast<uint8_t>(PatternType::RANDOM);
}

} // namespace PatternEngine

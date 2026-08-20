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

// How long an eased move of `distance` degrees must take to stay within this
// pattern's speed cap.
uint32_t moveDurationFor(const PatternParams &params, float distance) {
  const float speed = clampValue(params.randMaxSpeedDps, PATTERN_RANDOM_MIN_SPEED_DPS, PATTERN_RANDOM_MAX_SPEED_DPS);
  // Smoothstep's peak velocity is 1.5x its average, so stretch the move by
  // that factor: what the user sets as "max speed" is the actual peak the
  // servo sees mid-move, not an average it overshoots.
  // Rounded up, not truncated: shaving the fractional millisecond off would
  // make the move finish fractionally early, i.e. fractionally faster than
  // the speed cap the user set.
  uint32_t duration = static_cast<uint32_t>(ceilf((1.5f * distance / speed) * 1000.0f));
  return duration < PATTERN_RANDOM_MIN_MOVE_MS ? PATTERN_RANDOM_MIN_MOVE_MS : duration;
}

// Draws this cycle's interval, tolerating min/max entered the wrong way round.
uint32_t drawInterval(const PatternParams &params) {
  uint32_t lo = params.randMinIntervalMs;
  uint32_t hi = params.randMaxIntervalMs;
  if (hi < lo) {  // user entered them backwards — treat as an unordered pair
    const uint32_t swap = lo;
    lo = hi;
    hi = swap;
  }
  return randomIntervalMs(lo, hi);
}

// Picks one axis's next target; returns the move duration it needs.
uint32_t chooseTarget(const PatternParams &params, RandomPatternState &state, float minDeg, float maxDeg) {
  state.fromDeg = state.toDeg;
  state.toDeg = randomInRange(minDeg, maxDeg);
  return moveDurationFor(params, fabsf(state.toDeg - state.fromDeg));
}

// The interval covers the whole cycle (move + hold). A long move under a short
// interval would otherwise be cut off mid-travel by the next target, which is
// exactly the discontinuity the easing exists to avoid.
uint32_t cycleFor(uint32_t interval, uint32_t duration) {
  const uint32_t minCycle = duration + PATTERN_RANDOM_MIN_SETTLE_MS;
  return interval < minCycle ? minCycle : interval;
}

// Where an axis sits partway through its current eased move.
float easedPosition(const RandomPatternState &state, uint32_t t_ms) {
  const uint32_t elapsedInMove = t_ms - state.moveStartMs;
  if (state.moveDurationMs == 0 || elapsedInMove >= state.moveDurationMs) {
    return state.toDeg; // arrived: hold here until the next move is due
  }
  const float u = static_cast<float>(elapsedInMove) / static_cast<float>(state.moveDurationMs);
  return state.fromDeg + (state.toDeg - state.fromDeg) * smoothstep(u);
}

// Picks the next target and how long moving there should take, then schedules
// the move after it. Called once per cycle from computeRandomAngle().
void planNextRandomMove(const PatternParams &params, RandomPatternState &state, float minDeg, float maxDeg) {
  const uint32_t duration = chooseTarget(params, state, minDeg, maxDeg);
  const uint32_t interval = cycleFor(drawInterval(params), duration);
  state.moveStartMs = state.nextMoveMs;
  state.moveDurationMs = duration;
  state.nextMoveMs = state.moveStartMs + interval;
}

// Both axes at once, on one shared schedule. The X axis's interval settings
// own that schedule (Y's are ignored while linked) — one head can only look
// somewhere new at one rate, and taking the pair from one axis is easier to
// reason about than combining two ranges. Both axes ease over the same
// duration, set by whichever needs longer, so neither breaks its own speed cap
// and the head travels a straight line rather than dog-legging.
void planNextRandom2D(const PatternParams &paramsX, const PatternParams &paramsY, Random2DState &state,
                      float minX, float maxX, float minY, float maxY) {
  const uint32_t durX = chooseTarget(paramsX, state.x, minX, maxX);
  const uint32_t durY = chooseTarget(paramsY, state.y, minY, maxY);
  const uint32_t duration = durX > durY ? durX : durY;
  const uint32_t interval = cycleFor(drawInterval(paramsX), duration);

  state.x.moveStartMs = state.x.nextMoveMs;
  state.y.moveStartMs = state.x.moveStartMs; // one schedule, not two
  state.x.moveDurationMs = duration;
  state.y.moveDurationMs = duration;
  state.x.nextMoveMs = state.x.moveStartMs + interval;
  state.y.nextMoveMs = state.x.nextMoveMs;
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

  return easedPosition(state, t_ms);
}

void resetRandom2D(Random2DState &state, float currentX, float currentY) {
  resetRandom(state.x, currentX);
  resetRandom(state.y, currentY);
}

void computeRandom2D(const PatternParams &paramsX, const PatternParams &paramsY, Random2DState &state,
                     uint32_t t_ms, float minX, float maxX, float minY, float maxY, float *outX,
                     float *outY) {
  if (maxX < minX) { const float t = minX; minX = maxX; maxX = t; }
  if (maxY < minY) { const float t = minY; minY = maxY; maxY = t; }

  if (!state.x.initialized || !state.y.initialized) {
    state.x.initialized = true;
    state.y.initialized = true;
    state.x.fromDeg = clampValue(state.x.fromDeg, minX, maxX);
    state.y.fromDeg = clampValue(state.y.fromDeg, minY, maxY);
    state.x.toDeg = state.x.fromDeg;
    state.y.toDeg = state.y.fromDeg;
    state.x.moveStartMs = t_ms;
    state.y.moveStartMs = t_ms;
    state.x.moveDurationMs = 0;
    state.y.moveDurationMs = 0;
    state.x.nextMoveMs = t_ms; // first move is planned on this very tick
    state.y.nextMoveMs = t_ms;
  }

  // Signed comparison so this keeps scheduling across millis()' ~49-day wrap,
  // and a loop so a delayed tick catches up rather than stalling — same
  // reasoning as the single-axis path above. Only X's schedule is consulted:
  // planNextRandom2D keeps both in lockstep.
  while (static_cast<int32_t>(t_ms - state.x.nextMoveMs) >= 0) {
    planNextRandom2D(paramsX, paramsY, state, minX, maxX, minY, maxY);
  }

  if (outX) *outX = easedPosition(state.x, t_ms);
  if (outY) *outY = easedPosition(state.y, t_ms);
}

bool isValidType(uint8_t rawType) {
  return rawType <= static_cast<uint8_t>(PatternType::RANDOM);
}

} // namespace PatternEngine

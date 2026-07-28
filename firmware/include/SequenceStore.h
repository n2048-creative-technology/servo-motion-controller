#pragma once

#include <stdint.h>
#include "Config.h"

struct SequencePoint {
  uint32_t t_ms;
  int16_t angle_decideg; // angle * 10
};

// Fixed-capacity, no-heap-growth store for a manually recorded servo motion.
// Recording samples are captured at a fixed cadence (RECORD_INTERVAL_MS) by
// PlaybackEngine, independent of how fast the browser sends jog updates.
class SequenceStore {
public:
  void begin(); // mounts nothing itself, call loadFromFS() after LittleFS.begin()

  void startRecording();
  // Called at RECORD_INTERVAL_MS cadence while recording; ignored if buffer is full.
  void captureTick(float angleDeg, uint32_t elapsedMs);
  void stopRecording();
  bool isRecording() const { return recording_; }
  uint16_t recordedPointCount() const { return count_; }

  bool saveToFS();
  void discardRecording();

  bool loadFromFS();
  bool hasSequence() const { return loaded_ && count_ > 0; }
  uint16_t pointCount() const { return count_; }
  uint32_t durationMs() const { return durationMs_; }

  // Linear interpolation between bracketing points; t_ms wraps modulo durationMs.
  float angleAtTime(uint32_t t_ms) const;

private:
  SequencePoint points_[MAX_SEQ_POINTS];
  uint16_t count_ = 0;
  uint32_t durationMs_ = 0;
  bool recording_ = false;
  bool loaded_ = false;
};

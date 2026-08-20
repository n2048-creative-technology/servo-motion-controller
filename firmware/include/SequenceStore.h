#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Config.h"

// 8 bytes, and deliberately still 8 bytes now that it carries the relay
// state: `flags` and `reserved` occupy the two bytes the compiler was already
// inserting as padding after angle_decideg in v1. That's what keeps
// MAX_SEQ_POINTS' RAM budget (see Config.h) unchanged, and what lets a v1
// file be read back as-is — see SEQUENCE_FILE_VERSION.
struct SequencePoint {
  uint32_t t_ms;
  int16_t angle_decideg; // angle * 10
  uint8_t flags;         // bit 0: relay/light on at this instant
  uint8_t reserved;
};
static_assert(sizeof(SequencePoint) == 8, "SequencePoint must stay 8 bytes: MAX_SEQ_POINTS' RAM budget "
                                          "and the on-disk sequence format both assume it");

static constexpr uint8_t SEQ_FLAG_RELAY_ON = 0x01;

// Why saveAs() failed, if it did — surfaced over ESP-NOW as SeqAckStatus
// (see NetworkLink.h) and to the local web UI, so a failure reads as
// something actionable instead of a bare false.
enum class SaveResult : uint8_t {
  Ok = 0,
  NoPoints = 1,     // nothing was ever captured (recording never started, or was empty)
  InvalidName = 2,  // name sanitized to nothing
  WriteFailed = 3,  // LittleFS open/write failed (e.g. out of space)
};

// One sequence's metadata, as returned by listSequences() — cheap to produce
// since it's read from just each file's header, not its full point array.
struct SequenceInfo {
  char name[24] = {0};
  uint16_t points = 0;
  uint32_t durationMs = 0;
};

// Fixed-capacity, no-heap-growth store for a servo motion sequence — either
// hand-recorded locally (jog slider) or remotely captured via a Master's
// SEQ_START/SEQ_STOP (see NetworkLink.h). Recording samples are captured at
// a fixed cadence (RECORD_INTERVAL_MS) by PlaybackEngine, independent of how
// fast jog updates or network commands actually arrive.
//
// Sequences are stored on LittleFS as one file per name under SEQUENCE_DIR
// ("/seq/<name>.bin"), so a board can hold several and pick one for
// autostart or manual playback — not just the single fixed sequence v1 had.
class SequenceStore {
public:
  // Mounts nothing itself (call after LittleFS.begin()); ensures SEQUENCE_DIR
  // exists and migrates a v1-era single fixed-path sequence file into it
  // (once — a no-op on every boot after the first).
  void begin();

  void startRecording();
  // Called at RECORD_INTERVAL_MS cadence while recording; ignored if buffer is full.
  // The relay state is sampled alongside the angle, so a recording replays the
  // light exactly as it was switched during capture.
  void captureTick(float angleDeg, bool relayOn, uint32_t elapsedMs);
  void stopRecording();
  bool isRecording() const { return recording_; }
  uint16_t recordedPointCount() const { return count_; }

  // Writes the current buffer (just recorded, or previously loaded) as
  // "<name>.bin".
  SaveResult saveAs(const char *name);

  // Also usable for a free-space preflight check before recording/uploading.
  static uint32_t freeSpaceBytes();
  void discardRecording();

  // Loads a named sequence from FS into the active buffer (for playback).
  bool loadNamed(const char *name);
  bool hasSequence() const { return loaded_ && count_ > 0; }
  uint16_t pointCount() const { return count_; }
  uint32_t durationMs() const { return durationMs_; }
  const char *activeName() const { return activeName_; }

  // Linear interpolation between bracketing points; t_ms wraps modulo durationMs.
  float angleAtTime(uint32_t t_ms) const;

  // The relay state at that same instant. Held (not interpolated) from the
  // most recent point at or before t_ms — a relay is on or off, and blending
  // between the two would mean chattering it at the sample rate.
  bool relayAtTime(uint32_t t_ms) const;

  // Directory listing: fills `out[0..returned)` and returns how many were
  // found (capped at maxCount, and at SEQ_MAX_LISTED regardless).
  uint8_t listSequences(SequenceInfo *out, uint8_t maxCount) const;
  bool deleteSequence(const char *name);

  // Removes every saved sequence file on this board. Returns how many were
  // deleted. Also clears the active in-RAM buffer if it pointed at one of
  // them (matching deleteSequence's behavior for the active sequence).
  uint8_t clearAll();

  // Restricts to [A-Za-z0-9_-], truncates to SEQ_NAME_MAX_LEN chars. Returns
  // false (leaving `out` untouched) if nothing valid remains.
  static bool sanitizeName(const char *in, char *out, size_t outLen);

private:
  SequencePoint points_[MAX_SEQ_POINTS];
  uint16_t count_ = 0;
  uint32_t durationMs_ = 0;
  bool recording_ = false;
  bool loaded_ = false;
  char activeName_[24] = {0};

  static void pathFor(const char *name, char *out, size_t outLen);
  void migrateLegacyFile();
};

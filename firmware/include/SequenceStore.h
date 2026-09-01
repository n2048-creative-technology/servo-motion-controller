#pragma once

#include <stdint.h>
#include <stddef.h>
#include <LittleFS.h>

#include "Config.h"

// 12 bytes: both servo axes plus the light, with `flags`/`reserved` in what
// the compiler pads out after the two angles anyway. MAX_SEQ_POINTS came down
// when this grew from 8 bytes so the array's RAM footprint stayed put — see
// Config.h. The on-disk format assumes this layout too (v3).
struct SequencePoint {
  uint32_t t_ms;
  int16_t x_decideg; // X (pan) angle * 10
  int16_t y_decideg; // Y (tilt) angle * 10
  uint8_t flags;     // bit 0: relay/light on at this instant
  uint8_t reserved;
};
static_assert(sizeof(SequencePoint) == 12, "SequencePoint must stay 12 bytes: MAX_SEQ_POINTS' RAM budget "
                                           "and the on-disk sequence format both assume it");

// How a v1/v2 file's 8-byte record is laid out, for reading old sequences.
struct SequencePointV1 {
  uint32_t t_ms;
  int16_t angle_decideg;
  uint8_t flags;
  uint8_t reserved;
};
static_assert(sizeof(SequencePointV1) == SEQUENCE_POINT_BYTES_V1,
              "v1/v2 on-disk records are 8 bytes");

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
// hand-recorded locally (trackpad) or remotely captured via a Master's
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
  // Called at RECORD_INTERVAL_MS cadence while recording; ignored if buffer is
  // full. Both axes and the relay are sampled together, so a recording replays
  // the whole head — pan, tilt and light — exactly as it was performed.
  void captureTick(float xDeg, float yDeg, bool relayOn, uint32_t elapsedMs);
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

  // Everything at one instant, in a single pass: t_ms wraps modulo durationMs,
  // angles are linearly interpolated between the bracketing points, and the
  // relay is held (not interpolated) from the most recent point at or before
  // t_ms — a relay is on or off, and blending between the two would mean
  // chattering it at the sample rate.
  //
  // One call rather than three because the bracketing-point search is the
  // expensive part (a linear scan over up to MAX_SEQ_POINTS) and playback
  // needs all three channels on every 50Hz tick.
  void sampleAtTime(uint32_t t_ms, float *outX, float *outY, bool *outRelay) const;

  // False for a sequence recorded before this firmware had a Y axis: there is
  // no tilt track in the file, so playback must leave that axis alone rather
  // than driving it somewhere the recording never described.
  bool hasYTrack() const { return hasYTrack_; }

  // Appends a downsampled view of a saved sequence to `out` as compact JSON,
  // for plotting it in the web UI:
  //   {"name":..,"duration_ms":..,"has_y":..,"points":[[t,x,y,light],...]}
  //
  // Reads the file directly and never touches the active buffer, so looking at
  // one recording can't disturb another that's currently playing. Strided
  // rather than loaded whole: a full-length recording is thousands of points
  // and the plot is a few hundred pixels wide, so sending all of them would
  // cost far more RAM and airtime than the picture is worth. Works on
  // pre-Y-axis files too (they report has_y false).
  bool appendPlotJson(const char *name, uint16_t maxPoints, String &out) const;

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
  bool hasYTrack_ = true;
  char activeName_[24] = {0};

  static void pathFor(const char *name, char *out, size_t outLen);
  void migrateLegacyFile();
  // Reads a v1/v2 file's 8-byte records into the 12-byte in-RAM points.
  bool readLegacyPoints(File &f, uint16_t pointCount, uint16_t fileVersion);
};

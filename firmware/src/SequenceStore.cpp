#include "SequenceStore.h"

#include <LittleFS.h>

namespace {
struct FileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t pointCount;
  uint32_t durationMs;
};
} // namespace

void SequenceStore::begin() {
  count_ = 0;
  durationMs_ = 0;
  recording_ = false;
  loaded_ = false;
}

void SequenceStore::startRecording() {
  recording_ = true;
  count_ = 0;
  durationMs_ = 0;
}

void SequenceStore::captureTick(float angleDeg, uint32_t elapsedMs) {
  if (!recording_ || count_ >= MAX_SEQ_POINTS) return;
  points_[count_].t_ms = elapsedMs;
  points_[count_].angle_decideg = static_cast<int16_t>(angleDeg * 10.0f);
  count_++;
  durationMs_ = elapsedMs;
}

void SequenceStore::stopRecording() {
  recording_ = false;
}

void SequenceStore::discardRecording() {
  count_ = 0;
  durationMs_ = 0;
  recording_ = false;
}

bool SequenceStore::saveToFS() {
  if (count_ == 0) return false;

  File f = LittleFS.open(SEQUENCE_FILE_PATH, "w");
  if (!f) return false;

  FileHeader header{SEQUENCE_FILE_MAGIC, SEQUENCE_FILE_VERSION, count_, durationMs_};
  f.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  f.write(reinterpret_cast<const uint8_t *>(points_), sizeof(SequencePoint) * count_);
  f.close();

  loaded_ = true;
  return true;
}

bool SequenceStore::loadFromFS() {
  loaded_ = false;
  count_ = 0;
  durationMs_ = 0;

  if (!LittleFS.exists(SEQUENCE_FILE_PATH)) return false;

  File f = LittleFS.open(SEQUENCE_FILE_PATH, "r");
  if (!f) return false;

  FileHeader header;
  if (f.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header)) {
    f.close();
    return false;
  }
  if (header.magic != SEQUENCE_FILE_MAGIC || header.version != SEQUENCE_FILE_VERSION ||
      header.pointCount > MAX_SEQ_POINTS) {
    f.close();
    return false;
  }

  const size_t bytesToRead = sizeof(SequencePoint) * header.pointCount;
  const size_t got = f.read(reinterpret_cast<uint8_t *>(points_), bytesToRead);
  f.close();

  if (got != bytesToRead) return false;

  count_ = header.pointCount;
  durationMs_ = header.durationMs;
  loaded_ = true;
  return true;
}

float SequenceStore::angleAtTime(uint32_t t_ms) const {
  if (count_ == 0) return 0.0f;
  if (count_ == 1 || durationMs_ == 0) return points_[0].angle_decideg / 10.0f;

  const uint32_t wrapped = t_ms % durationMs_;

  for (uint16_t i = 0; i < count_ - 1; i++) {
    const SequencePoint &a = points_[i];
    const SequencePoint &b = points_[i + 1];
    if (wrapped >= a.t_ms && wrapped <= b.t_ms) {
      if (b.t_ms == a.t_ms) return a.angle_decideg / 10.0f;
      const float t = static_cast<float>(wrapped - a.t_ms) / static_cast<float>(b.t_ms - a.t_ms);
      const float angleA = a.angle_decideg / 10.0f;
      const float angleB = b.angle_decideg / 10.0f;
      return angleA + t * (angleB - angleA);
    }
  }

  // Wrap segment: interpolate from the last recorded point back to the first
  // point as time loops from durationMs_ back to 0.
  const SequencePoint &last = points_[count_ - 1];
  const SequencePoint &first = points_[0];
  const uint32_t wrapSpan = durationMs_ - last.t_ms;
  if (wrapSpan == 0) return last.angle_decideg / 10.0f;
  const float t = static_cast<float>(wrapped - last.t_ms) / static_cast<float>(wrapSpan);
  const float angleLast = last.angle_decideg / 10.0f;
  const float angleFirst = first.angle_decideg / 10.0f;
  return angleLast + t * (angleFirst - angleLast);
}

#include "SequenceStore.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

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
  activeName_[0] = '\0';

  if (!LittleFS.exists(SEQUENCE_DIR)) LittleFS.mkdir(SEQUENCE_DIR);
  migrateLegacyFile();
}

void SequenceStore::migrateLegacyFile() {
  char newPath[40];
  pathFor(SEQUENCE_LEGACY_MIGRATED_NAME, newPath, sizeof(newPath));
  if (LittleFS.exists(newPath)) return; // already migrated (or a "local" sequence already exists)
  if (!LittleFS.exists(SEQUENCE_LEGACY_FILE_PATH)) return; // nothing to migrate

  File in = LittleFS.open(SEQUENCE_LEGACY_FILE_PATH, "r");
  if (!in) return;
  File out = LittleFS.open(newPath, "w");
  if (!out) {
    in.close();
    return;
  }

  uint8_t buf[256];
  int n;
  while ((n = in.read(buf, sizeof(buf))) > 0) {
    out.write(buf, n);
  }
  in.close();
  out.close();
  LittleFS.remove(SEQUENCE_LEGACY_FILE_PATH);
  Serial.printf("[SEQ] migrated legacy sequence file to %s\n", newPath);
}

void SequenceStore::pathFor(const char *name, char *out, size_t outLen) {
  snprintf(out, outLen, "%s/%s.bin", SEQUENCE_DIR, name);
}

bool SequenceStore::sanitizeName(const char *in, char *out, size_t outLen) {
  if (!in || outLen == 0) return false;
  size_t j = 0;
  for (size_t i = 0; in[i] != '\0' && j < outLen - 1 && j < SEQ_NAME_MAX_LEN; i++) {
    char c = in[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (ok) out[j++] = c;
  }
  out[j] = '\0';
  return j > 0;
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

SaveResult SequenceStore::saveAs(const char *name) {
  if (count_ == 0) return SaveResult::NoPoints;
  char safeName[SEQ_NAME_MAX_LEN + 1];
  if (!sanitizeName(name, safeName, sizeof(safeName))) return SaveResult::InvalidName;

  char path[40];
  pathFor(safeName, path, sizeof(path));

  File f = LittleFS.open(path, "w");
  if (!f) return SaveResult::WriteFailed;

  FileHeader header{SEQUENCE_FILE_MAGIC, SEQUENCE_FILE_VERSION, count_, durationMs_};
  f.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
  f.write(reinterpret_cast<const uint8_t *>(points_), sizeof(SequencePoint) * count_);
  f.close();

  strncpy(activeName_, safeName, sizeof(activeName_) - 1);
  activeName_[sizeof(activeName_) - 1] = '\0';
  loaded_ = true;
  return SaveResult::Ok;
}

uint32_t SequenceStore::freeSpaceBytes() {
  return LittleFS.totalBytes() - LittleFS.usedBytes();
}

bool SequenceStore::loadNamed(const char *name) {
  loaded_ = false;
  count_ = 0;
  durationMs_ = 0;

  char safeName[SEQ_NAME_MAX_LEN + 1];
  if (!sanitizeName(name, safeName, sizeof(safeName))) return false;

  char path[40];
  pathFor(safeName, path, sizeof(path));
  if (!LittleFS.exists(path)) return false;

  File f = LittleFS.open(path, "r");
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
  strncpy(activeName_, safeName, sizeof(activeName_) - 1);
  activeName_[sizeof(activeName_) - 1] = '\0';
  loaded_ = true;
  return true;
}

uint8_t SequenceStore::listSequences(SequenceInfo *out, uint8_t maxCount) const {
  uint8_t found = 0;
  const uint8_t cap = maxCount < SEQ_MAX_LISTED ? maxCount : SEQ_MAX_LISTED;

  File dir = LittleFS.open(SEQUENCE_DIR);
  if (!dir || !dir.isDirectory()) return 0;

  File f = dir.openNextFile();
  while (f && found < cap) {
    if (!f.isDirectory()) {
      // f.name() is just the basename ("foo.bin") on this core; strip the extension.
      String base = f.name();
      int dot = base.lastIndexOf(".bin");
      if (dot > 0) {
        FileHeader header;
        if (f.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) == sizeof(header) &&
            header.magic == SEQUENCE_FILE_MAGIC && header.version == SEQUENCE_FILE_VERSION) {
          String name = base.substring(0, dot);
          strncpy(out[found].name, name.c_str(), sizeof(out[found].name) - 1);
          out[found].name[sizeof(out[found].name) - 1] = '\0';
          out[found].points = header.pointCount;
          out[found].durationMs = header.durationMs;
          found++;
        }
      }
    }
    f = dir.openNextFile();
  }
  return found;
}

bool SequenceStore::deleteSequence(const char *name) {
  char safeName[SEQ_NAME_MAX_LEN + 1];
  if (!sanitizeName(name, safeName, sizeof(safeName))) return false;

  char path[40];
  pathFor(safeName, path, sizeof(path));
  if (!LittleFS.remove(path)) return false;

  if (strcmp(safeName, activeName_) == 0) {
    loaded_ = false;
    count_ = 0;
    durationMs_ = 0;
    activeName_[0] = '\0';
  }
  return true;
}

uint8_t SequenceStore::clearAll() {
  uint8_t removed = 0;
  // One directory pass per delete (rather than deleting while iterating an
  // open directory handle, which isn't well-defined on this LittleFS
  // wrapper) — fine given a board only ever holds a handful of sequences.
  for (;;) {
    char name[SEQ_NAME_MAX_LEN + 1] = {0};
    bool found = false;

    File dir = LittleFS.open(SEQUENCE_DIR);
    if (!dir || !dir.isDirectory()) break;
    File f = dir.openNextFile();
    while (f) {
      if (!f.isDirectory()) {
        String base = f.name();
        int dot = base.lastIndexOf(".bin");
        if (dot > 0) {
          String n = base.substring(0, dot);
          strncpy(name, n.c_str(), sizeof(name) - 1);
          name[sizeof(name) - 1] = '\0';
          found = true;
          break;
        }
      }
      f = dir.openNextFile();
    }
    dir.close();

    if (!found) break;
    if (!deleteSequence(name)) break; // avoid looping forever on a delete that keeps failing
    removed++;
  }
  return removed;
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

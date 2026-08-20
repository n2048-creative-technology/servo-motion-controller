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
  hasYTrack_ = true; // anything captured from here on records both axes
}

void SequenceStore::captureTick(float xDeg, float yDeg, bool relayOn, uint32_t elapsedMs) {
  if (!recording_ || count_ >= MAX_SEQ_POINTS) return;
  points_[count_].t_ms = elapsedMs;
  points_[count_].x_decideg = static_cast<int16_t>(xDeg * 10.0f);
  points_[count_].y_decideg = static_cast<int16_t>(yDeg * 10.0f);
  points_[count_].flags = relayOn ? SEQ_FLAG_RELAY_ON : 0;
  points_[count_].reserved = 0;
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
  hasYTrack_ = true;

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
  if (header.magic != SEQUENCE_FILE_MAGIC || header.version < SEQUENCE_FILE_VERSION_MIN ||
      header.version > SEQUENCE_FILE_VERSION || header.pointCount > MAX_SEQ_POINTS) {
    f.close();
    return false;
  }

  if (header.version < SEQUENCE_FILE_VERSION_XY) {
    // Pre-Y file: 8-byte records that have to be widened into the 12-byte
    // in-RAM points, and no tilt data at all to widen them with.
    if (!readLegacyPoints(f, header.pointCount, header.version)) {
      f.close();
      return false;
    }
    hasYTrack_ = false;
  } else {
    const size_t bytesToRead = sizeof(SequencePoint) * header.pointCount;
    const size_t got = f.read(reinterpret_cast<uint8_t *>(points_), bytesToRead);
    if (got != bytesToRead) {
      f.close();
      return false;
    }
    hasYTrack_ = true;
  }
  f.close();

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
            header.magic == SEQUENCE_FILE_MAGIC && header.version >= SEQUENCE_FILE_VERSION_MIN &&
            header.version <= SEQUENCE_FILE_VERSION) {
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

// v1/v2 files store 8-byte records; the in-RAM point is 12. Read them in
// batches into a small stack buffer and widen each one, rather than either
// bloating the RAM budget with a second full-size array or doing one tiny
// filesystem read per point.
bool SequenceStore::readLegacyPoints(File &f, uint16_t pointCount, uint16_t fileVersion) {
  static constexpr uint16_t kBatch = 64;
  SequencePointV1 batch[kBatch];

  uint16_t done = 0;
  while (done < pointCount) {
    const uint16_t want = (pointCount - done) < kBatch ? (pointCount - done) : kBatch;
    const size_t bytes = sizeof(SequencePointV1) * want;
    if (f.read(reinterpret_cast<uint8_t *>(batch), bytes) != static_cast<int>(bytes)) return false;
    for (uint16_t i = 0; i < want; i++) {
      SequencePoint &dst = points_[done + i];
      dst.t_ms = batch[i].t_ms;
      dst.x_decideg = batch[i].angle_decideg;
      dst.y_decideg = 0; // no tilt track — see hasYTrack()
      // v1 never initialized the byte v2 later used for relay flags, so
      // trusting it would replay an old recording with the light flickering
      // at random.
      dst.flags = fileVersion >= 2 ? (batch[i].flags & SEQ_FLAG_RELAY_ON) : 0;
      dst.reserved = 0;
    }
    done += want;
  }
  return true;
}

void SequenceStore::sampleAtTime(uint32_t t_ms, float *outX, float *outY, bool *outRelay) const {
  if (outX) *outX = 0.0f;
  if (outY) *outY = 0.0f;
  if (outRelay) *outRelay = false;
  if (count_ == 0) return;

  if (count_ == 1 || durationMs_ == 0) {
    if (outX) *outX = points_[0].x_decideg / 10.0f;
    if (outY) *outY = points_[0].y_decideg / 10.0f;
    if (outRelay) *outRelay = (points_[0].flags & SEQ_FLAG_RELAY_ON) != 0;
    return;
  }

  const uint32_t wrapped = t_ms % durationMs_;

  for (uint16_t i = 0; i < count_ - 1; i++) {
    const SequencePoint &a = points_[i];
    const SequencePoint &b = points_[i + 1];
    if (wrapped >= a.t_ms && wrapped <= b.t_ms) {
      // The relay is held (never blended) from the most recent point at or
      // before `wrapped` — which is `b` when we've landed exactly on its
      // timestamp, and `a` anywhere in between. Taking it from `a`
      // unconditionally would delay every light change by a whole sample
      // interval, since a switch is recorded at the instant it happens.
      const SequencePoint &holder = wrapped >= b.t_ms ? b : a;
      if (outRelay) *outRelay = (holder.flags & SEQ_FLAG_RELAY_ON) != 0;
      if (b.t_ms == a.t_ms) {
        if (outX) *outX = a.x_decideg / 10.0f;
        if (outY) *outY = a.y_decideg / 10.0f;
        return;
      }
      const float t = static_cast<float>(wrapped - a.t_ms) / static_cast<float>(b.t_ms - a.t_ms);
      if (outX) *outX = (a.x_decideg + t * (b.x_decideg - a.x_decideg)) / 10.0f;
      if (outY) *outY = (a.y_decideg + t * (b.y_decideg - a.y_decideg)) / 10.0f;
      return;
    }
  }

  // Wrap segment: interpolate from the last recorded point back to the first
  // as time loops from durationMs_ back to 0.
  const SequencePoint &last = points_[count_ - 1];
  const SequencePoint &first = points_[0];
  if (outRelay) *outRelay = (last.flags & SEQ_FLAG_RELAY_ON) != 0;
  const uint32_t wrapSpan = durationMs_ - last.t_ms;
  if (wrapSpan == 0) {
    if (outX) *outX = last.x_decideg / 10.0f;
    if (outY) *outY = last.y_decideg / 10.0f;
    return;
  }
  const float t = static_cast<float>(wrapped - last.t_ms) / static_cast<float>(wrapSpan);
  if (outX) *outX = (last.x_decideg + t * (first.x_decideg - last.x_decideg)) / 10.0f;
  if (outY) *outY = (last.y_decideg + t * (first.y_decideg - last.y_decideg)) / 10.0f;
}

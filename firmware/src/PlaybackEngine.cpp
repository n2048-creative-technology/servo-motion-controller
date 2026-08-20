#include "PlaybackEngine.h"

#include "SequenceStore.h"
#include "Config.h"

void PlaybackEngine::begin(IMotionSink *sink, SequenceStore *sequence, IRelaySink *relay) {
  servos_ = sink;
  sequence_ = sequence;
  relay_ = relay;
  mode_ = PlaybackMode::IDLE;
}

void PlaybackEngine::setAngleLimits(float minX, float maxX, float minY, float maxY) {
  minXDeg_ = minX;
  maxXDeg_ = maxX;
  minYDeg_ = minY;
  maxYDeg_ = maxY;
}

void PlaybackEngine::computePattern(uint32_t elapsed, float *outX, float *outY) {
  if (randomLinked()) {
    // Both axes RANDOM: one schedule, one coherent (X,Y) target per interval.
    PatternEngine::computeRandom2D(patternX_, patternY_, randomState_, elapsed, minXDeg_, maxXDeg_,
                                   minYDeg_, maxYDeg_, outX, outY);
    return;
  }
  // Otherwise each axis is evaluated on its own — including one axis being
  // RANDOM while the other runs an ordinary waveform.
  *outX = patternX_.type == PatternType::RANDOM
              ? PatternEngine::computeRandomAngle(patternX_, randomState_.x, elapsed, minXDeg_, maxXDeg_)
              : PatternEngine::computeAngle(patternX_, elapsed);
  *outY = patternY_.type == PatternType::RANDOM
              ? PatternEngine::computeRandomAngle(patternY_, randomState_.y, elapsed, minYDeg_, maxYDeg_)
              : PatternEngine::computeAngle(patternY_, elapsed);
}

void PlaybackEngine::tick(uint32_t now) {
  switch (mode_) {
    case PlaybackMode::IDLE:
      break;

    case PlaybackMode::MANUAL:
    case PlaybackMode::NETWORK:
      // The servos' PWM already holds whatever angles were last commanded in
      // hardware, but periodically re-write them anyway: cheap insurance that
      // self-heals a Node back to the right position on its own (no fresh
      // jog/network command required) if anything ever left it out of sync.
      if (now - lastReapplyMs_ >= SERVO_REAPPLY_INTERVAL_MS) {
        lastReapplyMs_ = now;
        servos_->writeAngles(servos_->getX(), servos_->getY());
      }
      break;

    case PlaybackMode::RECORDING:
      if (now - lastRecordCaptureMs_ >= RECORD_INTERVAL_MS) {
        sequence_->captureTick(servos_->getX(), servos_->getY(), relayState(), now - modeStartMs_);
        lastRecordCaptureMs_ = now;
      }
      break;

    case PlaybackMode::PATTERN: {
      float x = 0.0f, y = 0.0f;
      computePattern(now - modeStartMs_, &x, &y);
      servos_->writeAngles(x, y);
      break;
    }

    case PlaybackMode::SEQUENCE: {
      const uint32_t elapsed = now - modeStartMs_;
      float x = 0.0f, y = 0.0f;
      bool relayOn = false;
      sequence_->sampleAtTime(elapsed, &x, &y, &relayOn);
      // A sequence recorded before there was a Y axis has no Y track at all —
      // hold that axis where it is rather than driving it to a position the
      // recording never captured.
      servos_->writeAngles(x, sequence_->hasYTrack() ? y : servos_->getY());
      // Playback owns the light for as long as it runs — the recording's own
      // relay track is replayed alongside its motion. Both sinks ignore a
      // write that doesn't change anything, so this doesn't chatter the relay
      // (or, on a Master, spam ESP-NOW) at the 50 Hz tick rate.
      if (relay_) relay_->writeRelay(relayOn);
      break;
    }
  }
}

void PlaybackEngine::onJog(float xDeg, float yDeg, uint32_t now) {
  servos_->writeAngles(xDeg, yDeg);
  if (mode_ != PlaybackMode::RECORDING) {
    mode_ = PlaybackMode::MANUAL;
  }
  (void)now;
}

void PlaybackEngine::onNetworkCommand(float xDeg, float yDeg, bool relayOn, uint32_t now) {
  servos_->writeAngles(xDeg, yDeg);
  if (relay_) relay_->writeRelay(relayOn);
  if (mode_ != PlaybackMode::RECORDING) {
    mode_ = PlaybackMode::NETWORK;
  }
  (void)now;
}

void PlaybackEngine::onRelayToggle(bool on) {
  if (relay_) relay_->writeRelay(on);
}

void PlaybackEngine::startPattern(const PatternParams &paramsX, const PatternParams &paramsY, uint32_t now) {
  patternX_ = paramsX;
  patternY_ = paramsY;
  // Starts from wherever the head is pointing right now, so the first random
  // move eases out of the current position instead of jumping to it.
  PatternEngine::resetRandom2D(randomState_, servos_->getX(), servos_->getY());
  mode_ = PlaybackMode::PATTERN;
  modeStartMs_ = now;
}

void PlaybackEngine::stopPattern() {
  if (mode_ == PlaybackMode::PATTERN) mode_ = PlaybackMode::MANUAL;
}

void PlaybackEngine::startRecording(uint32_t now) {
  sequence_->startRecording();
  mode_ = PlaybackMode::RECORDING;
  modeStartMs_ = now;
  lastRecordCaptureMs_ = now;
  sequence_->captureTick(servos_->getX(), servos_->getY(), relayState(), 0);
}

void PlaybackEngine::stopRecording() {
  sequence_->stopRecording();
  if (mode_ == PlaybackMode::RECORDING) mode_ = PlaybackMode::MANUAL;
}

void PlaybackEngine::startSequencePlayback(uint32_t now) {
  if (!sequence_->hasSequence()) return;
  mode_ = PlaybackMode::SEQUENCE;
  modeStartMs_ = now;
}

void PlaybackEngine::stopSequencePlayback() {
  if (mode_ == PlaybackMode::SEQUENCE) mode_ = PlaybackMode::MANUAL;
}

void PlaybackEngine::applyAutostart(const PersistedSettings &settings, uint32_t now) {
  if (!settings.autostartEnabled) {
    mode_ = PlaybackMode::IDLE;
    return;
  }

  if (settings.autostartTarget == AutostartTarget::PATTERN) {
    startPattern(settings.autostartPatternX, settings.autostartPatternY, now);
  } else if (settings.autostartTarget == AutostartTarget::SEQUENCE) {
    sequence_->loadNamed(settings.autostartSequenceName);
    startSequencePlayback(now);
  }
}

#include "PlaybackEngine.h"

#include "SequenceStore.h"
#include "Config.h"

void PlaybackEngine::begin(IAngleSink *sink, SequenceStore *sequence, IRelaySink *relay) {
  servo_ = sink;
  sequence_ = sequence;
  relay_ = relay;
  mode_ = PlaybackMode::IDLE;
}

void PlaybackEngine::setAngleLimits(float minDeg, float maxDeg) {
  minAngleDeg_ = minDeg;
  maxAngleDeg_ = maxDeg;
}

void PlaybackEngine::tick(uint32_t now) {
  switch (mode_) {
    case PlaybackMode::IDLE:
      break;

    case PlaybackMode::MANUAL:
    case PlaybackMode::NETWORK:
      // The servo's PWM already holds whatever angle was last commanded in
      // hardware, but periodically re-write it anyway: cheap insurance that
      // self-heals a Node back to the right position on its own (no fresh
      // jog/network command required) if anything ever left it out of sync.
      if (now - lastReapplyMs_ >= SERVO_REAPPLY_INTERVAL_MS) {
        lastReapplyMs_ = now;
        servo_->writeAngle(servo_->getAngle());
      }
      break;

    case PlaybackMode::RECORDING:
      if (now - lastRecordCaptureMs_ >= RECORD_INTERVAL_MS) {
        sequence_->captureTick(servo_->getAngle(), relayState(), now - modeStartMs_);
        lastRecordCaptureMs_ = now;
      }
      break;

    case PlaybackMode::PATTERN: {
      const uint32_t elapsed = now - modeStartMs_;
      const float angle =
          activePattern_.type == PatternType::RANDOM
              ? PatternEngine::computeRandomAngle(activePattern_, randomState_, elapsed, minAngleDeg_, maxAngleDeg_)
              : PatternEngine::computeAngle(activePattern_, elapsed);
      servo_->writeAngle(angle);
      break;
    }

    case PlaybackMode::SEQUENCE: {
      const uint32_t elapsed = now - modeStartMs_;
      servo_->writeAngle(sequence_->angleAtTime(elapsed));
      // Playback owns the light for as long as it runs — the recording's own
      // relay track is replayed alongside its motion. Both sinks ignore a
      // write that doesn't change anything, so this doesn't chatter the relay
      // (or, on a Master, spam ESP-NOW) at the 50 Hz tick rate.
      if (relay_) relay_->writeRelay(sequence_->relayAtTime(elapsed));
      break;
    }
  }
}

void PlaybackEngine::onJog(float angleDeg, uint32_t now) {
  servo_->writeAngle(angleDeg);
  if (mode_ != PlaybackMode::RECORDING) {
    mode_ = PlaybackMode::MANUAL;
  }
  (void)now;
}

void PlaybackEngine::onNetworkCommand(float angleDeg, bool relayOn, uint32_t now) {
  servo_->writeAngle(angleDeg);
  if (relay_) relay_->writeRelay(relayOn);
  if (mode_ != PlaybackMode::RECORDING) {
    mode_ = PlaybackMode::NETWORK;
  }
  (void)now;
}

void PlaybackEngine::onRelayToggle(bool on) {
  if (relay_) relay_->writeRelay(on);
}

void PlaybackEngine::startPattern(const PatternParams &params, uint32_t now) {
  activePattern_ = params;
  // Starts from wherever the servo is right now, so the first random move
  // eases out of the current position instead of jumping to it.
  PatternEngine::resetRandom(randomState_, servo_->getAngle());
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
  sequence_->captureTick(servo_->getAngle(), relayState(), 0);
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
    startPattern(settings.autostartPattern, now);
  } else if (settings.autostartTarget == AutostartTarget::SEQUENCE) {
    sequence_->loadNamed(settings.autostartSequenceName);
    startSequencePlayback(now);
  }
}

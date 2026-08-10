#include "PlaybackEngine.h"

#include "SequenceStore.h"
#include "Config.h"

void PlaybackEngine::begin(IAngleSink *sink, SequenceStore *sequence) {
  servo_ = sink;
  sequence_ = sequence;
  mode_ = PlaybackMode::IDLE;
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
        sequence_->captureTick(servo_->getAngle(), now - modeStartMs_);
        lastRecordCaptureMs_ = now;
      }
      break;

    case PlaybackMode::PATTERN: {
      const float angle = PatternEngine::computeAngle(activePattern_, now - modeStartMs_);
      servo_->writeAngle(angle);
      break;
    }

    case PlaybackMode::SEQUENCE: {
      const float angle = sequence_->angleAtTime(now - modeStartMs_);
      servo_->writeAngle(angle);
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

void PlaybackEngine::onNetworkCommand(float angleDeg, uint32_t now) {
  servo_->writeAngle(angleDeg);
  if (mode_ != PlaybackMode::RECORDING) {
    mode_ = PlaybackMode::NETWORK;
  }
  (void)now;
}

void PlaybackEngine::startPattern(const PatternParams &params, uint32_t now) {
  activePattern_ = params;
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
  sequence_->captureTick(servo_->getAngle(), 0);
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

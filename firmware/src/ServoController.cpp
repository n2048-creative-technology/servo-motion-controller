#include "ServoController.h"
#include "Config.h"

void ServoController::begin(uint8_t pin, uint16_t minUs, uint16_t maxUs, float minAngle, float maxAngle,
                             bool invert) {
  pin_ = pin;
  minUs_ = minUs;
  maxUs_ = maxUs;
  minAngle_ = minAngle;
  maxAngle_ = maxAngle;
  invert_ = invert;

  // One LEDC timer is enough for a single servo on the C3.
  ESP32PWM::allocateTimer(0);
  servo_.setPeriodHertz(50);
  servo_.attach(pin_, minUs_, maxUs_);
  attached_ = true;

  currentAngle_ = (minAngle_ + maxAngle_) / 2.0f;
  writeAngle(currentAngle_);
}

void ServoController::writeAngle(float degrees) {
  if (!attached_) return;
  const float clamped = clampValue(degrees, minAngle_, maxAngle_);
  currentAngle_ = clamped;

  // ESP32Servo's write(int) clamps internally to 0-180 degrees and only treats
  // values >=500 as raw microseconds, so it cannot drive wide-range (e.g. 270°)
  // servos correctly. Map degrees -> pulse width ourselves and go straight to
  // writeMicroseconds() to support the full calibrated angle range.
  const float span = maxAngle_ - minAngle_;
  float t = span != 0.0f ? (clamped - minAngle_) / span : 0.0f;
  if (invert_) t = 1.0f - t;
  const int pulseUs = minUs_ + static_cast<int>(t * static_cast<float>(maxUs_ - minUs_));
  servo_.writeMicroseconds(pulseUs);
}

void ServoController::setCalibration(uint16_t minUs, uint16_t maxUs, float minAngle, float maxAngle) {
  minUs_ = minUs;
  maxUs_ = maxUs;
  minAngle_ = minAngle;
  maxAngle_ = maxAngle;
  if (attached_) {
    servo_.detach();
    servo_.attach(pin_, minUs_, maxUs_);
    writeAngle(currentAngle_);
  }
}

void ServoController::setInvert(bool invert) {
  invert_ = invert;
  if (attached_) writeAngle(currentAngle_);
}

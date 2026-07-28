#pragma once

#include <ESP32Servo.h>
#include <stdint.h>
#include "IAngleSink.h"

class ServoController : public IAngleSink {
public:
  void begin(uint8_t pin, uint16_t minUs, uint16_t maxUs, float minAngle, float maxAngle);

  // Clamps to the calibrated angle range and writes the pulse.
  void writeAngle(float degrees) override;

  float getAngle() const override { return currentAngle_; }

  void setCalibration(uint16_t minUs, uint16_t maxUs, float minAngle, float maxAngle);

  uint16_t minUs() const { return minUs_; }
  uint16_t maxUs() const { return maxUs_; }
  float minAngle() const { return minAngle_; }
  float maxAngle() const { return maxAngle_; }

private:
  Servo servo_;
  uint8_t pin_ = 0;
  uint16_t minUs_ = 500;
  uint16_t maxUs_ = 2500;
  float minAngle_ = 0.0f;
  float maxAngle_ = 270.0f;
  float currentAngle_ = 0.0f;
  bool attached_ = false;
};

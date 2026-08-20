#pragma once

#include "IMotionSink.h"
#include "ServoController.h"

// The two physical servos of one pan/tilt head, presented to PlaybackEngine
// as a single motion sink. Each axis keeps its own independent calibration
// (pulse range, angle range, centre, invert) — a tilt axis is usually a
// different servo with a different mechanical range from the pan axis, so
// sharing one calibration between them was never going to work.
class ServoPair : public IMotionSink {
public:
  void writeAngles(float xDeg, float yDeg) override;
  float getX() const override { return x_.getAngle(); }
  float getY() const override { return y_.getAngle(); }

  // Direct access for setup and for the calibration routes in WebApi.
  ServoController &x() { return x_; }
  ServoController &y() { return y_; }
  const ServoController &x() const { return x_; }
  const ServoController &y() const { return y_; }

private:
  ServoController x_;
  ServoController y_;
};

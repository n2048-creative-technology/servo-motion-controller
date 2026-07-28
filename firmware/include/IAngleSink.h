#pragma once

#include <stdint.h>
#include <stddef.h>

// What PlaybackEngine drives: "move to this angle" / "what angle are we at".
// ServoController implements this for a physical servo; NetworkAngleSink
// implements it for a Master board, turning the same jog/pattern/sequence
// logic into ESP-NOW CMD broadcasts to a selected Node (or all of them)
// instead of a local PWM pulse.
class IAngleSink {
public:
  virtual ~IAngleSink() = default;

  virtual void writeAngle(float degrees) = 0;
  virtual float getAngle() const = 0;

  // Node targeting (Master only): which Node(s) does writeAngle() actually
  // reach? Default no-ops/trivial values so ServoController doesn't need to
  // implement them; NetworkAngleSink overrides all four.
  virtual void setTargets(bool broadcastAll, const uint8_t *ids, size_t count) {
    (void)broadcastAll;
    (void)ids;
    (void)count;
  }
  virtual bool targetsBroadcastAll() const { return true; }
  virtual size_t targetCount() const { return 0; }
  virtual uint8_t targetAt(size_t index) const {
    (void)index;
    return 0;
  }
};

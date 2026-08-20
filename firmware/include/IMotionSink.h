#pragma once

#include <stdint.h>
#include <stddef.h>

// What PlaybackEngine drives: "move the head to this X/Y" / "where is it
// now". ServoPair implements this for two physical servos (pan on D10, tilt
// on D3); NetworkMotionSink implements it for a Master board, turning the
// same jog/pattern/sequence logic into ESP-NOW CMD broadcasts to a selected
// Node (or all of them) instead of local PWM pulses.
//
// Both axes always travel together in one call, rather than one sink call per
// axis: a Master turns each call into a packet, and splitting X and Y across
// two packets would let a Node act on half a move — visibly dog-legging on
// its way to a diagonal position, and recording that dog-leg into any
// sequence being captured.
class IMotionSink {
public:
  virtual ~IMotionSink() = default;

  virtual void writeAngles(float xDeg, float yDeg) = 0;
  virtual float getX() const = 0;
  virtual float getY() const = 0;

  // Node targeting (Master only): which Node(s) does writeAngles() actually
  // reach? Default no-ops/trivial values so ServoPair doesn't need to
  // implement them; NetworkMotionSink overrides all four.
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

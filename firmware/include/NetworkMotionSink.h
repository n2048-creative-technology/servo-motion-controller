#pragma once

#include <vector>

#include "IMotionSink.h"
#include "IRelaySink.h"
#include "NetworkLink.h"
#include "Config.h"

// Master-only IMotionSink: instead of local PWM pulses, "moving" means
// broadcasting an ESP-NOW CMD to whichever Node(s) are currently selected —
// all of them, or an explicit list. Lets PlaybackEngine's existing
// jog/pattern/sequence logic drive remote Nodes from the Master's own web UI
// unchanged; a multi-node selection is a client-side fan-out (one CMD send
// per target), not a new wire format.
//
// It's the relay sink too, for the same reason: on a Master the light toggle
// has to reach the selected Node(s) rather than the Master's own D7 pin, so
// it rides along on the very same CMD packets the angle does.
class NetworkMotionSink : public IMotionSink, public IRelaySink {
public:
  void begin(NetworkLink *network) { network_ = network; }

  void writeAngles(float xDeg, float yDeg) override {
    lastX_ = xDeg;
    lastY_ = yDeg;
    sendToTargets(xDeg, yDeg);
  }

  float getX() const override { return lastX_; }
  float getY() const override { return lastY_; }

  void writeRelay(bool on) override {
    if (on == relayOn_) return;
    relayOn_ = on;
    // The state itself travels on CMD packets, so push the current angle back
    // out to carry it immediately instead of waiting up to
    // NET_CMD_RESEND_INTERVAL_MS for the next periodic resend. Only the
    // currently-selected target(s) are touched: the Master tracks relay state
    // per target, so this can't disturb a Node that some other control (a
    // joystick button, a PC tool) is switching independently.
    sendToTargets(lastX_, lastY_);
  }

  bool relayState() const override { return relayOn_; }

  void setTargets(bool broadcastAll, const uint8_t *ids, size_t count) override {
    broadcastAll_ = broadcastAll;
    targets_.assign(ids, ids + count);
  }
  bool targetsBroadcastAll() const override { return broadcastAll_; }
  size_t targetCount() const override { return targets_.size(); }
  uint8_t targetAt(size_t index) const override {
    return index < targets_.size() ? targets_[index] : 0;
  }

private:
  void sendToTargets(float xDeg, float yDeg) {
    if (!network_) return;
    if (broadcastAll_ || targets_.empty()) {
      network_->sendCommand(NET_BROADCAST_NODE, xDeg, yDeg, relayOn_);
    } else {
      for (uint8_t id : targets_) network_->sendCommand(id, xDeg, yDeg, relayOn_);
    }
  }

  NetworkLink *network_ = nullptr;
  bool broadcastAll_ = true;
  std::vector<uint8_t> targets_;
  float lastX_ = 0.0f;
  float lastY_ = 0.0f;
  bool relayOn_ = false;
};

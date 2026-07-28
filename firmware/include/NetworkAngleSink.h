#pragma once

#include <vector>

#include "IAngleSink.h"
#include "NetworkLink.h"
#include "Config.h"

// Master-only IAngleSink: instead of a local PWM pulse, "moving" means
// broadcasting an ESP-NOW CMD to whichever Node(s) are currently selected —
// all of them, or an explicit list. Lets PlaybackEngine's existing
// jog/pattern/sequence logic drive remote Nodes from the Master's own web UI
// unchanged; a multi-node selection is a client-side fan-out (one CMD send
// per target), not a new wire format.
class NetworkAngleSink : public IAngleSink {
public:
  void begin(NetworkLink *network) { network_ = network; }

  void writeAngle(float degrees) override {
    lastAngle_ = degrees;
    if (!network_) return;
    if (broadcastAll_ || targets_.empty()) {
      network_->sendCommand(NET_BROADCAST_NODE, degrees);
    } else {
      for (uint8_t id : targets_) network_->sendCommand(id, degrees);
    }
  }

  float getAngle() const override { return lastAngle_; }

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
  NetworkLink *network_ = nullptr;
  bool broadcastAll_ = true;
  std::vector<uint8_t> targets_;
  float lastAngle_ = 0.0f;
};

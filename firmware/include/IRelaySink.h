#pragma once

// The relay/light counterpart to IAngleSink: "switch the light on/off" /
// "is it on right now". RelayController implements it for the physical D7
// output; NetworkAngleSink implements it for a Master board, turning the same
// toggle into ESP-NOW CMD packets aimed at the selected Node(s) instead of a
// local GPIO write — so the relay follows exactly the same local-vs-remote
// routing the jog slider already does.
class IRelaySink {
public:
  virtual ~IRelaySink() = default;

  virtual void writeRelay(bool on) = 0;
  virtual bool relayState() const = 0;
};

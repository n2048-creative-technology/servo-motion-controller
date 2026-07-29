#pragma once

#include <stdint.h>
#include <functional>

#include "Config.h"
#include "SettingsStore.h"

// Fixed-size POD sent raw over ESP-NOW (well under its 250-byte payload cap).
// CMD: Master -> Node(s), addressed by targetNode (0 = broadcast to all).
// HELLO: Node -> Master heartbeat, carries the sender's own id + angle.
struct NetPacket {
  uint8_t magic = NET_PACKET_MAGIC;
  uint8_t version = NET_PACKET_VERSION;
  uint8_t type = 0;        // 1=CMD, 2=HELLO
  uint8_t targetNode = 0;  // CMD only: 0=all nodes, else specific node id
  uint8_t nodeId = 0;      // HELLO only: sender's own node id
  float angleDeg = 0.0f;   // CMD: target angle / HELLO: current angle
};

static constexpr uint8_t NET_PACKET_TYPE_CMD = 1;
static constexpr uint8_t NET_PACKET_TYPE_HELLO = 2;

struct KnownNode {
  uint8_t id = 0;
  float angleDeg = 0.0f;
  uint32_t lastSeenMs = 0;
  bool inUse = false;
};

// Master: the last angle sent to a given target (a specific node id, or 0
// for "all nodes"), so it can be periodically re-sent — see
// NET_CMD_RESEND_INTERVAL_MS.
struct LastCommand {
  uint8_t targetNode = 0;
  float angleDeg = 0.0f;
  uint32_t lastSentMs = 0;
  bool inUse = false;
};

// ESP-NOW transport shared by MASTER and NODE roles. Must be started only
// after WiFi (the board's own AP) is already up, since ESP-NOW rides on top
// of the WiFi driver and needs a fixed channel to reach other boards.
class NetworkLink {
public:
  // No-op when mode == STANDALONE.
  void begin(OperatingMode mode, uint8_t nodeId);

  // Call every loop() iteration. Sends the periodic HELLO heartbeat in NODE
  // mode, and in MASTER mode re-sends any target's last command that's
  // overdue for a refresh (see NET_CMD_RESEND_INTERVAL_MS); receiving
  // happens via ESP-NOW's own callback either way.
  void loopTick(uint32_t now);

  // NODE only: feed the servo's actual current angle each loop() so the
  // HELLO heartbeat reports real position, regardless of what's driving it
  // (jog, pattern, sequence, or a prior network command).
  void reportLocalAngle(float angleDeg) { localAngle_ = angleDeg; }

  // MASTER only: broadcast a CMD packet addressed to targetNode (0 = all).
  bool sendCommand(uint8_t targetNode, float angleDeg);

  // NODE only: invoked when a CMD addressed to us (or to "all") arrives.
  void onNodeCommand(std::function<void(float angleDeg)> callback) { nodeCommandCb_ = callback; }

  // MASTER only: read-only view of the known-node table for WebApi/SerialBridge.
  const KnownNode *knownNodes() const { return knownNodes_; }
  static constexpr uint8_t maxKnownNodes() { return NET_MAX_TRACKED_NODES; }

private:
  OperatingMode mode_ = OperatingMode::STANDALONE;
  uint8_t nodeId_ = 0;
  uint32_t lastHelloMs_ = 0;
  float localAngle_ = 0.0f;
  bool espNowReady_ = false;

  std::function<void(float angleDeg)> nodeCommandCb_;
  KnownNode knownNodes_[NET_MAX_TRACKED_NODES];
  LastCommand lastCommands_[NET_MAX_LAST_COMMANDS];

  void recordHello(uint8_t fromNodeId, float angleDeg, uint32_t now);
  void recordLastCommand(uint8_t targetNode, float angleDeg, uint32_t now);
  bool transmitCommand(uint8_t targetNode, float angleDeg);
  void resendDueCommands(uint32_t now);

public:
  // Called from the ESP-NOW C callback trampoline; not part of the public
  // API surface otherwise (needs to be reachable from a free function).
  void onRecv(const uint8_t *data, int len);
};

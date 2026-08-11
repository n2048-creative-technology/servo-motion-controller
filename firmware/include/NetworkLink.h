#pragma once

#include <stdint.h>
#include <functional>

#include "Config.h"
#include "SettingsStore.h"

// Fixed-size POD sent raw over ESP-NOW (well under its 250-byte payload cap).
// CMD: Master -> Node(s), addressed by targetNode (0 = broadcast to all).
// HELLO: Node -> Master heartbeat, carries the sender's own id + angle.
// SEQ_START/SEQ_STOP: Master -> a specific Node, remotely triggers/finishes a
// recording exactly like that Node's own /api/record/start|save would — used
// to "upload" a sequence: the Master starts it recording, then streams
// ordinary CMD packets (which drive the servo *and* get captured, since
// RECORDING mode already survives network-driven moves — see
// PlaybackEngine::onNetworkCommand), then SEQ_STOP tells it to save the
// result under a name. SEQ_ACK: Node -> Master, reports whether that save
// succeeded (and how many points), relayed to the PC over serial.
//
// sessionId/seq (CMD only) let a Node tell a stale/delayed packet apart from
// the current one: sessionId is randomized once when a Master boots, seq
// increments on every CMD it sends (fresh commands and periodic resends
// alike). Without this, a resend queued before a fresher command but
// delivered after it (plausible once the radio link is degraded/congested —
// exactly when resends are most likely to matter) would yank the servo back
// to a stale angle instead of being ignored. SEQ_START/STOP don't carry
// this — they're rare one-shot actions, not a continuous stream, so the PC
// tool just retries on a timeout instead.
struct NetPacket {
  uint8_t magic = NET_PACKET_MAGIC;
  uint8_t version = NET_PACKET_VERSION;
  uint8_t type = 0;        // 1=CMD, 2=HELLO, 3=SEQ_START, 4=SEQ_STOP, 5=SEQ_ACK
  uint8_t targetNode = 0;  // CMD/SEQ_START/SEQ_STOP: 0=all nodes, else specific node id
  uint8_t nodeId = 0;      // HELLO/SEQ_ACK only: sender's own node id
  float angleDeg = 0.0f;   // CMD: target angle / HELLO: current angle
  uint32_t sessionId = 0;  // CMD only: randomized per Master boot
  uint32_t seq = 0;        // CMD only: monotonically increasing per session
  char seqName[24] = {0};  // SEQ_STOP/SEQ_ACK only: sequence name, sanitized+truncated
  uint8_t status = 0;      // SEQ_ACK only: 0 = saved OK, nonzero = failed
  uint16_t pointCount = 0; // SEQ_ACK only: points captured/saved
};

static constexpr uint8_t NET_PACKET_TYPE_CMD = 1;
static constexpr uint8_t NET_PACKET_TYPE_HELLO = 2;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_START = 3;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_STOP = 4;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_ACK = 5;

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

  // NODE only: invoked when a SEQ_START/SEQ_STOP addressed to us arrives.
  void onSeqStart(std::function<void()> callback) { seqStartCb_ = callback; }
  void onSeqStop(std::function<void(const char *name)> callback) { seqStopCb_ = callback; }

  // MASTER only: invoked when a SEQ_ACK arrives (fromNodeId, name, ok, pointCount).
  void onSeqAck(std::function<void(uint8_t, const char *, bool, uint16_t)> callback) {
    seqAckCb_ = callback;
  }

  // MASTER only: remotely start/stop-and-save a recording on targetNode.
  bool sendSeqStart(uint8_t targetNode);
  bool sendSeqStop(uint8_t targetNode, const char *name);

  // NODE only: reports the outcome of a SEQ_STOP back to the Master.
  bool sendSeqAck(const char *name, bool ok, uint16_t pointCount);

  // MASTER only: read-only view of the known-node table for WebApi/SerialBridge.
  const KnownNode *knownNodes() const { return knownNodes_; }
  static constexpr uint8_t maxKnownNodes() { return NET_MAX_TRACKED_NODES; }

private:
  OperatingMode mode_ = OperatingMode::STANDALONE;
  uint8_t nodeId_ = 0;
  uint32_t lastHelloMs_ = 0;
  float localAngle_ = 0.0f;
  bool espNowReady_ = false;

  // Master: identifies this boot so a Node can tell "Master rebooted, seq
  // restarted from 0" apart from "this is just a stale/reordered packet".
  uint32_t sessionId_ = 0;
  uint32_t nextSeq_ = 0;

  // Node: highest (sessionId, seq) applied so far, to reject anything older.
  uint32_t lastSessionId_ = 0;
  uint32_t lastAppliedSeq_ = 0;
  bool haveSession_ = false;

  std::function<void(float angleDeg)> nodeCommandCb_;
  std::function<void()> seqStartCb_;
  std::function<void(const char *name)> seqStopCb_;
  std::function<void(uint8_t, const char *, bool, uint16_t)> seqAckCb_;
  KnownNode knownNodes_[NET_MAX_TRACKED_NODES];
  LastCommand lastCommands_[NET_MAX_LAST_COMMANDS];

  // esp_now_register_recv_cb's callback runs on the WiFi driver's own
  // FreeRTOS task, not loop()'s — invoking seqAckCb_ directly from onRecv()
  // would call SerialBridge::reportUploadResult()'s Serial.println() from
  // that task while loop() concurrently writes its own replies on Serial,
  // with nothing serializing the two: the two lines' bytes can interleave
  // into corrupted JSON the PC side can't parse (observed in practice — an
  // upload's own success confirmation coming back garbled and unparsed).
  // Stashing the ack here and firing seqAckCb_ from loopTick() instead
  // keeps every Serial write on loop()'s task.
  bool pendingSeqAck_ = false;
  uint8_t pendingAckNodeId_ = 0;
  char pendingAckName_[24] = {0};
  bool pendingAckOk_ = false;
  uint16_t pendingAckPoints_ = 0;

  // Same reasoning as pendingSeqAck_ above, but for the Node side: onSeqStop
  // triggers SequenceStore::saveAs(), a full LittleFS file write that can
  // take a while for a long recording (up to ~96KB at the current cap).
  // Doing that — or even just mutating PlaybackEngine/SequenceStore state
  // from onSeqStart — directly from the WiFi driver's own task instead of
  // loop()'s risks starving that task long enough to trip the watchdog and
  // reboot the board (observed in practice: a crash landing right as a long
  // upload's save begins). Deferring both to loopTick() keeps all of that
  // work on loop()'s task, same as every other SequenceStore/PlaybackEngine
  // access in this codebase.
  bool pendingSeqStart_ = false;
  bool pendingSeqStop_ = false;
  char pendingStopName_[24] = {0};

  void recordHello(uint8_t fromNodeId, float angleDeg, uint32_t now);
  void recordLastCommand(uint8_t targetNode, float angleDeg, uint32_t now);
  bool transmitCommand(uint8_t targetNode, float angleDeg);
  void resendDueCommands(uint32_t now);

public:
  // Called from the ESP-NOW C callback trampoline; not part of the public
  // API surface otherwise (needs to be reachable from a free function).
  void onRecv(const uint8_t *data, int len);
};

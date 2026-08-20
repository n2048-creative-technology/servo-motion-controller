#pragma once

#include <stdint.h>
#include <functional>

#include "Config.h"
#include "SettingsStore.h"

// Fixed-size POD sent raw over ESP-NOW (well under its 250-byte payload cap).
// CMD: Master -> Node(s), addressed by targetNode (0 = broadcast to all).
// HELLO: Node -> Master heartbeat, carries the sender's own id, both servo
// axes and its light state.
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
  float angleX = 0.0f;     // CMD: target X (pan) / HELLO: current X
  float angleY = 0.0f;     // CMD: target Y (tilt) / HELLO: current Y
  uint32_t sessionId = 0;  // CMD only: randomized per Master boot
  uint32_t seq = 0;        // CMD only: monotonically increasing per session
  char seqName[24] = {0};  // SEQ_STOP/SEQ_ACK only: sequence name, sanitized+truncated
  uint8_t status = 0;      // SEQ_ACK only: 0 = saved OK, nonzero = a SeqAckStatus reason code
  uint16_t pointCount = 0; // SEQ_ACK only: points captured/saved
  uint32_t freeBytes = 0;  // SPACE_REPLY only: bytes free in the Node's LittleFS
  // CMD: the relay/light state that goes with this angle. Carried on every
  // CMD rather than sent as its own packet type so it inherits the angle's
  // resend/ordering guarantees for free — and so a Node recording a
  // Master-driven sequence captures the light exactly in step with the
  // motion, which a separate best-effort packet couldn't promise.
  // HELLO: the sender's own current relay state, so a Master's node table
  // (and the PC tools reading it) can show each Node's actual light.
  uint8_t relayOn = 0;
};

static constexpr uint8_t NET_PACKET_TYPE_CMD = 1;
static constexpr uint8_t NET_PACKET_TYPE_HELLO = 2;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_START = 3;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_STOP = 4;
static constexpr uint8_t NET_PACKET_TYPE_SEQ_ACK = 5;
// Master -> a specific Node (or broadcast): delete every saved sequence on
// it, same effect as that Node's own /api/sequences/clear. Fire-and-forget
// like SEQ_START — no ack, since it's a one-shot housekeeping step (e.g.
// "clear before uploading") rather than data that needs a delivery
// guarantee; the PC tool just gives it a moment to land before proceeding.
static constexpr uint8_t NET_PACKET_TYPE_SEQ_CLEAR = 6;
// Master -> a specific Node: "how much LittleFS space do you have free?" —
// used as an upload preflight check. SPACE_REPLY: Node -> Master, carries
// freeBytes.
static constexpr uint8_t NET_PACKET_TYPE_SPACE_QUERY = 7;
static constexpr uint8_t NET_PACKET_TYPE_SPACE_REPLY = 8;

// SEQ_ACK's status byte when saving failed, distinguishing *why* — surfaced
// to the PC as human-readable text (see SerialBridge::reportUploadResult)
// instead of a bare "ok:false" that gives no hint whether to just retry or
// look at something else (weak signal vs. a Node that reset mid-upload vs.
// its flash genuinely being full).
enum class SeqAckStatus : uint8_t {
  Ok = 0,
  NoPointsCaptured = 1, // recording never started (SEQ_START lost, or the Node reset mid-transfer)
  InvalidName = 2,      // name sanitized to nothing
  WriteFailed = 3,      // LittleFS open/write failed (e.g. out of space)
};

struct KnownNode {
  uint8_t id = 0;
  float angleX = 0.0f;
  float angleY = 0.0f;
  bool relayOn = false;
  uint32_t lastSeenMs = 0;
  bool inUse = false;
};

// Master: the last angle sent to a given target (a specific node id, or 0
// for "all nodes"), so it can be periodically re-sent — see
// NET_CMD_RESEND_INTERVAL_MS.
struct LastCommand {
  uint8_t targetNode = 0;
  float angleX = 0.0f;
  float angleY = 0.0f;
  bool relayOn = false;
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

  // NODE only: feed both servos' actual current angles each loop() so the
  // HELLO heartbeat reports real position, regardless of what's driving them
  // (jog, pattern, sequence, or a prior network command).
  void reportLocalAngles(float angleX, float angleY) {
    localX_ = angleX;
    localY_ = angleY;
  }

  // NODE only: same idea for the relay, so the HELLO heartbeat reports the
  // light's real state no matter what switched it (web UI, a Master's CMD, or
  // sequence playback).
  void reportLocalRelay(bool on) { localRelay_ = on; }

  // MASTER only: broadcast a CMD packet addressed to targetNode (0 = all),
  // carrying both servo axes and the relay/light state for that target.
  //
  // Both axes travel in one packet so a Node can never act on half a move:
  // splitting them would let it dog-leg to a diagonal position, and record
  // that dog-leg into any sequence being captured.
  //
  // Relay state is tracked *per target* (alongside the angle, in
  // lastCommands_) rather than as one Master-wide value: a controller with a
  // button per Node has to be able to switch one Node's light without
  // touching another's, and a single shared flag would leak onto every other
  // target's periodic resend. Note that targetNode 0 (broadcast) is its own
  // entry, so a broadcast command carries the broadcast entry's relay state
  // to every Node — mix broadcast and per-node commands with that in mind.
  bool sendCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn);

  // MASTER only: what was last sent to that target (defaults if it has never
  // been commanded). Lets a caller that only wants to change one thing resend
  // the rest as-is rather than guessing: moving a Node without disturbing its
  // light, or driving pan without snapping tilt to some default.
  bool lastRelayFor(uint8_t targetNode) const;
  float lastXFor(uint8_t targetNode) const;
  float lastYFor(uint8_t targetNode) const;

  // NODE only: invoked when a CMD addressed to us (or to "all") arrives.
  void onNodeCommand(std::function<void(float angleX, float angleY, bool relayOn)> callback) {
    nodeCommandCb_ = callback;
  }

  // NODE only: invoked when a SEQ_START/SEQ_STOP/SEQ_CLEAR/SPACE_QUERY addressed to us arrives.
  void onSeqStart(std::function<void()> callback) { seqStartCb_ = callback; }
  void onSeqStop(std::function<void(const char *name)> callback) { seqStopCb_ = callback; }
  void onSeqClear(std::function<void()> callback) { seqClearCb_ = callback; }
  void onSpaceQuery(std::function<void()> callback) { spaceQueryCb_ = callback; }

  // MASTER only: invoked when a SEQ_ACK arrives (fromNodeId, name, status, pointCount).
  void onSeqAck(std::function<void(uint8_t, const char *, SeqAckStatus, uint16_t)> callback) {
    seqAckCb_ = callback;
  }

  // MASTER only: invoked when a SPACE_REPLY arrives (fromNodeId, freeBytes).
  void onSpaceReply(std::function<void(uint8_t, uint32_t)> callback) { spaceReplyCb_ = callback; }

  // MASTER only: remotely start/stop-and-save a recording on targetNode.
  bool sendSeqStart(uint8_t targetNode);
  bool sendSeqStop(uint8_t targetNode, const char *name);

  // MASTER only: remotely delete every saved sequence on targetNode.
  bool sendSeqClear(uint8_t targetNode);

  // MASTER only: ask targetNode how much LittleFS space it has free.
  bool sendSpaceQuery(uint8_t targetNode);

  // NODE only: reports the outcome of a SEQ_STOP back to the Master.
  bool sendSeqAck(const char *name, SeqAckStatus status, uint16_t pointCount);

  // NODE only: reports free LittleFS space back to the Master.
  bool sendSpaceReply(uint32_t freeBytes);

  // MASTER only: read-only view of the known-node table for WebApi/SerialBridge.
  const KnownNode *knownNodes() const { return knownNodes_; }
  static constexpr uint8_t maxKnownNodes() { return NET_MAX_TRACKED_NODES; }

private:
  OperatingMode mode_ = OperatingMode::STANDALONE;
  uint8_t nodeId_ = 0;
  uint32_t lastHelloMs_ = 0;
  float localX_ = 0.0f;
  float localY_ = 0.0f;
  bool espNowReady_ = false;

  // Master: identifies this boot so a Node can tell "Master rebooted, seq
  // restarted from 0" apart from "this is just a stale/reordered packet".
  uint32_t sessionId_ = 0;
  uint32_t nextSeq_ = 0;

  // Node: highest (sessionId, seq) applied so far, to reject anything older.
  uint32_t lastSessionId_ = 0;
  uint32_t lastAppliedSeq_ = 0;
  bool haveSession_ = false;

  bool localRelay_ = false; // Node: relay state reported in its HELLO

  std::function<void(float angleX, float angleY, bool relayOn)> nodeCommandCb_;
  std::function<void()> seqStartCb_;
  std::function<void(const char *name)> seqStopCb_;
  std::function<void()> seqClearCb_;
  std::function<void()> spaceQueryCb_;
  std::function<void(uint8_t, const char *, SeqAckStatus, uint16_t)> seqAckCb_;
  std::function<void(uint8_t, uint32_t)> spaceReplyCb_;
  KnownNode knownNodes_[NET_MAX_TRACKED_NODES];
  LastCommand lastCommands_[NET_MAX_LAST_COMMANDS];

  // esp_now_register_recv_cb's callback runs on the WiFi driver's own
  // FreeRTOS task, not loop()'s — invoking seqAckCb_ directly from onRecv()
  // would call SerialBridge::reportUploadResult()'s Serial.println() from
  // that task while loop() concurrently writes its own replies on Serial,
  // with nothing serializing the two: the two lines' bytes can interleave
  // into corrupted JSON the PC side can't parse (observed in practice — an
  // upload's own success confirmation coming back garbled and unparsed).
  // Stashing acks here and firing seqAckCb_ from loopTick() instead keeps
  // every Serial write on loop()'s task. This is a small FIFO, not a single
  // slot, because "upload to all Nodes" now runs several Nodes' uploads
  // concurrently — a single slot let a second Node's SEQ_ACK silently
  // clobber a first, still-undrained one, losing its confirmation entirely
  // (observed in practice: concurrent multi-Node uploads reproducibly
  // losing every ack even though every Node actually saved successfully).
  struct PendingAck {
    uint8_t nodeId = 0;
    char name[24] = {0};
    SeqAckStatus status = SeqAckStatus::Ok;
    uint16_t points = 0;
  };
  PendingAck pendingAcks_[NET_MAX_PENDING_ACKS];
  uint8_t pendingAckCount_ = 0;

  // Same FIFO reasoning as pendingAcks_ above — several Nodes' SPACE_REPLYs
  // can land close together during a multi-Node upload's preflight check.
  struct PendingSpaceReply {
    uint8_t nodeId = 0;
    uint32_t freeBytes = 0;
  };
  PendingSpaceReply pendingSpaceReplies_[NET_MAX_PENDING_ACKS];
  uint8_t pendingSpaceReplyCount_ = 0;

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
  bool pendingSeqClear_ = false;
  bool pendingSpaceQuery_ = false;

  const LastCommand *findLastCommand(uint8_t targetNode) const;
  void recordHello(uint8_t fromNodeId, float angleX, float angleY, bool relayOn, uint32_t now);
  void recordLastCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn, uint32_t now);
  bool transmitCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn);
  void resendDueCommands(uint32_t now);

public:
  // Called from the ESP-NOW C callback trampoline; not part of the public
  // API surface otherwise (needs to be reachable from a free function).
  void onRecv(const uint8_t *data, int len);
};

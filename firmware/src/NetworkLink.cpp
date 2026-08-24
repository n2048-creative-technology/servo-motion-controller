#include "NetworkLink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <string.h>
#include <cmath>

namespace {
// esp_now_register_recv_cb takes a plain function pointer, so a single
// static instance pointer bridges the C callback back into the object.
// Fine in practice: exactly one NetworkLink is ever constructed (in main.cpp).
NetworkLink *g_instance = nullptr;

// esp_now_recv_cb_t's signature changed across arduino-esp32 core versions
// (older cores pass the sender's raw MAC address here; newer ones pass an
// esp_now_recv_info_t*). This project pins framework-arduinoespressif32
// 3.20017 (see platformio.ini's lock file), which still uses the older,
// plain-MAC signature — matching it exactly here, rather than the newer
// esp_now_recv_info_t one, is what actually compiles against what's
// installed.
void onRecvTrampoline(const uint8_t *mac, const uint8_t *data, int len) {
  (void)mac;
  if (g_instance) g_instance->onRecv(data, len);
}

const uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
} // namespace

void NetworkLink::begin(OperatingMode mode, uint8_t nodeId) {
  mode_ = mode;
  nodeId_ = nodeId;
  if (mode_ == OperatingMode::STANDALONE) return;

  g_instance = this;

  if (esp_now_init() != ESP_OK) {
    Serial.println("[NET] esp_now_init FAILED");
    return;
  }

  esp_now_peer_info_t peer{};
  memcpy(peer.peer_addr, kBroadcastMac, 6);
  peer.channel = AP_WIFI_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_AP;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[NET] esp_now_add_peer(broadcast) FAILED");
    return;
  }

  esp_now_register_recv_cb(onRecvTrampoline);
  espNowReady_ = true;

  if (mode_ == OperatingMode::MASTER) {
    sessionId_ = esp_random();
    nextSeq_ = 0;
  }

  Serial.printf("[NET] esp-now ready, role=%s node_id=%u\n",
                mode_ == OperatingMode::MASTER ? "master" : "node", nodeId_);
}

void NetworkLink::loopTick(uint32_t now) {
  if (!espNowReady_) return;

  if (mode_ == OperatingMode::NODE && now - lastHelloMs_ >= NET_HELLO_INTERVAL_MS) {
    lastHelloMs_ = now;
    NetPacket pkt;
    pkt.type = NET_PACKET_TYPE_HELLO;
    pkt.nodeId = nodeId_;
    pkt.angleX = localX_;
    pkt.angleY = localY_;
    pkt.relayOn = localRelay_ ? 1 : 0;
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
  } else if (mode_ == OperatingMode::MASTER) {
    resendDueCommands(now);
  }

  while (pendingAckCount_ > 0) {
    PendingAck ack = pendingAcks_[0];
    for (uint8_t i = 1; i < pendingAckCount_; i++) pendingAcks_[i - 1] = pendingAcks_[i];
    pendingAckCount_--;
    if (seqAckCb_) seqAckCb_(ack.nodeId, ack.name, ack.status, ack.points);
  }

  while (pendingSpaceReplyCount_ > 0) {
    PendingSpaceReply reply = pendingSpaceReplies_[0];
    for (uint8_t i = 1; i < pendingSpaceReplyCount_; i++) pendingSpaceReplies_[i - 1] = pendingSpaceReplies_[i];
    pendingSpaceReplyCount_--;
    if (spaceReplyCb_) spaceReplyCb_(reply.nodeId, reply.freeBytes);
  }

  if (pendingSeqStart_) {
    pendingSeqStart_ = false;
    if (seqStartCb_) seqStartCb_();
  }
  if (pendingSeqStop_) {
    pendingSeqStop_ = false;
    if (seqStopCb_) seqStopCb_(pendingStopName_);
  }
  if (pendingSeqClear_) {
    pendingSeqClear_ = false;
    if (seqClearCb_) seqClearCb_();
  }
  if (pendingSpaceQuery_) {
    pendingSpaceQuery_ = false;
    if (spaceQueryCb_) spaceQueryCb_();
  }
}

bool NetworkLink::transmitCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn) {
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_CMD;
  pkt.targetNode = targetNode;
  pkt.angleX = angleX;
  pkt.angleY = angleY;
  pkt.relayOn = relayOn ? 1 : 0;
  pkt.sessionId = sessionId_;
  pkt.seq = nextSeq_++;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  const bool ok = transmitCommand(targetNode, angleX, angleY, relayOn);
  if (ok) recordLastCommand(targetNode, angleX, angleY, relayOn, millis());
  return ok;
}

bool NetworkLink::lastRelayFor(uint8_t targetNode) const {
  const LastCommand *cmd = findLastCommand(targetNode);
  return cmd ? cmd->relayOn : false;
}

float NetworkLink::lastXFor(uint8_t targetNode) const {
  const LastCommand *cmd = findLastCommand(targetNode);
  return cmd ? cmd->angleX : SERVO_DEFAULT_CENTER_ANGLE;
}

float NetworkLink::lastYFor(uint8_t targetNode) const {
  const LastCommand *cmd = findLastCommand(targetNode);
  return cmd ? cmd->angleY : SERVO_DEFAULT_CENTER_ANGLE;
}

const LastCommand *NetworkLink::findLastCommand(uint8_t targetNode) const {
  for (uint8_t i = 0; i < NET_MAX_LAST_COMMANDS; i++) {
    if (lastCommands_[i].inUse && lastCommands_[i].targetNode == targetNode) {
      return &lastCommands_[i];
    }
  }
  return nullptr;
}

void NetworkLink::recordLastCommand(uint8_t targetNode, float angleX, float angleY, bool relayOn,
                                    uint32_t now) {
  int freeSlot = -1;
  for (uint8_t i = 0; i < NET_MAX_LAST_COMMANDS; i++) {
    if (lastCommands_[i].inUse && lastCommands_[i].targetNode == targetNode) {
      lastCommands_[i].angleX = angleX;
      lastCommands_[i].angleY = angleY;
      lastCommands_[i].relayOn = relayOn;
      lastCommands_[i].lastSentMs = now;
      return;
    }
    if (freeSlot < 0 && !lastCommands_[i].inUse) freeSlot = i;
  }
  // Table full and this is a never-before-seen target: reuse slot 0 rather
  // than dropping the update silently. Distinct concurrent targets beyond
  // NET_MAX_LAST_COMMANDS aren't expected in practice (one per Node/broadcast).
  const int slot = freeSlot >= 0 ? freeSlot : 0;
  lastCommands_[slot].targetNode = targetNode;
  lastCommands_[slot].angleX = angleX;
  lastCommands_[slot].angleY = angleY;
  lastCommands_[slot].relayOn = relayOn;
  lastCommands_[slot].lastSentMs = now;
  lastCommands_[slot].inUse = true;
}

bool NetworkLink::sendSeqStart(uint8_t targetNode) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SEQ_START;
  pkt.targetNode = targetNode;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendSeqStop(uint8_t targetNode, const char *name) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SEQ_STOP;
  pkt.targetNode = targetNode;
  strncpy(pkt.seqName, name, sizeof(pkt.seqName) - 1);
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendSeqClear(uint8_t targetNode) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SEQ_CLEAR;
  pkt.targetNode = targetNode;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendSeqAck(const char *name, SeqAckStatus status, uint16_t pointCount) {
  if (!espNowReady_ || mode_ != OperatingMode::NODE) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SEQ_ACK;
  pkt.nodeId = nodeId_;
  strncpy(pkt.seqName, name, sizeof(pkt.seqName) - 1);
  pkt.status = static_cast<uint8_t>(status);
  pkt.pointCount = pointCount;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendSpaceQuery(uint8_t targetNode) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SPACE_QUERY;
  pkt.targetNode = targetNode;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendSpaceReply(uint32_t freeBytes) {
  if (!espNowReady_ || mode_ != OperatingMode::NODE) return false;
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_SPACE_REPLY;
  pkt.nodeId = nodeId_;
  pkt.freeBytes = freeBytes;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

void NetworkLink::resendDueCommands(uint32_t now) {
  for (uint8_t i = 0; i < NET_MAX_LAST_COMMANDS; i++) {
    LastCommand &cmd = lastCommands_[i];
    if (!cmd.inUse) continue;
    if (now - cmd.lastSentMs >= NET_CMD_RESEND_INTERVAL_MS) {
      transmitCommand(cmd.targetNode, cmd.angleX, cmd.angleY, cmd.relayOn);
      cmd.lastSentMs = now;
    }
  }
}

void NetworkLink::recordHello(uint8_t fromNodeId, float angleX, float angleY, bool relayOn,
                              uint32_t now) {
  int freeSlot = -1;
  for (uint8_t i = 0; i < NET_MAX_TRACKED_NODES; i++) {
    if (knownNodes_[i].inUse && knownNodes_[i].id == fromNodeId) {
      knownNodes_[i].angleX = angleX;
      knownNodes_[i].angleY = angleY;
      knownNodes_[i].relayOn = relayOn;
      knownNodes_[i].lastSeenMs = now;
      return;
    }
    if (freeSlot < 0 && !knownNodes_[i].inUse) freeSlot = i;
  }
  if (freeSlot >= 0) {
    KnownNode &slot = knownNodes_[freeSlot];
    slot.id = fromNodeId;
    slot.angleX = angleX;
    slot.angleY = angleY;
    slot.relayOn = relayOn;
    slot.lastSeenMs = now;
    slot.inUse = true;
  }
}

void NetworkLink::onRecv(const uint8_t *data, int len) {
  if (len != sizeof(NetPacket)) return;
  NetPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != NET_PACKET_MAGIC) return;
  if (pkt.version != NET_PACKET_VERSION) {
    // Otherwise silent and easy to mistake for "that board is out of range":
    // a board left off a firmware update round talks a protocol version this
    // one doesn't recognize and gets ignored with no other symptom.
    Serial.printf("[NET] ignoring packet with protocol version %u (this board expects %u) — "
                  "that sender needs reflashing to match\n",
                  pkt.version, NET_PACKET_VERSION);
    return;
  }

  const uint32_t now = millis();

  if (mode_ == OperatingMode::MASTER && pkt.type == NET_PACKET_TYPE_HELLO) {
    recordHello(pkt.nodeId, pkt.angleX, pkt.angleY, pkt.relayOn != 0, now);
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_CMD) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    // Corrupted/garbage payload: never let this reach a servo. Both axes are
    // checked, since one bad float would otherwise still be written.
    if (!isfinite(pkt.angleX) || !isfinite(pkt.angleY)) return;

    // Reject anything older than the last command we already applied, so a
    // resend or retransmit that got delayed in transit can't yank the servo
    // back to a stale angle after a fresher one already arrived. A changed
    // sessionId means the Master rebooted (its seq restarted from 0), so
    // that always wins over whatever session we'd been tracking.
    const bool newSession = !haveSession_ || pkt.sessionId != lastSessionId_;
    const bool newerInSession = !newSession && (int32_t)(pkt.seq - lastAppliedSeq_) > 0;
    if (!newSession && !newerInSession) return;

    haveSession_ = true;
    lastSessionId_ = pkt.sessionId;
    lastAppliedSeq_ = pkt.seq;
    if (nodeCommandCb_) nodeCommandCb_(pkt.angleX, pkt.angleY, pkt.relayOn != 0);
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_SEQ_START) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    // Deferred to loopTick() — see the pendingSeqStart_/pendingSeqStop_
    // fields' comment in NetworkLink.h.
    pendingSeqStart_ = true;
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_SEQ_STOP) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    pkt.seqName[sizeof(pkt.seqName) - 1] = '\0'; // defensive: never trust a wire string is terminated
    strncpy(pendingStopName_, pkt.seqName, sizeof(pendingStopName_) - 1);
    pendingStopName_[sizeof(pendingStopName_) - 1] = '\0';
    pendingSeqStop_ = true;
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_SEQ_CLEAR) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    pendingSeqClear_ = true;
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_SPACE_QUERY) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    pendingSpaceQuery_ = true;
  } else if (mode_ == OperatingMode::MASTER && pkt.type == NET_PACKET_TYPE_SEQ_ACK) {
    pkt.seqName[sizeof(pkt.seqName) - 1] = '\0';
    // Queued for loopTick() rather than calling seqAckCb_ here directly —
    // see the pendingAcks_ fields' comment in NetworkLink.h.
    if (pendingAckCount_ < NET_MAX_PENDING_ACKS) {
      PendingAck &slot = pendingAcks_[pendingAckCount_++];
      slot.nodeId = pkt.nodeId;
      strncpy(slot.name, pkt.seqName, sizeof(slot.name) - 1);
      slot.name[sizeof(slot.name) - 1] = '\0';
      slot.status = static_cast<SeqAckStatus>(pkt.status);
      slot.points = pkt.pointCount;
    }
    // else: more acks arrived than loop() has drained — drop the overflow
    // rather than corrupt an existing slot; the PC side already retries a
    // SEQ_STOP it never got confirmation for.
  } else if (mode_ == OperatingMode::MASTER && pkt.type == NET_PACKET_TYPE_SPACE_REPLY) {
    if (pendingSpaceReplyCount_ < NET_MAX_PENDING_ACKS) {
      PendingSpaceReply &slot = pendingSpaceReplies_[pendingSpaceReplyCount_++];
      slot.nodeId = pkt.nodeId;
      slot.freeBytes = pkt.freeBytes;
    }
  }
}

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
    pkt.angleDeg = localAngle_;
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
  } else if (mode_ == OperatingMode::MASTER) {
    resendDueCommands(now);
  }
}

bool NetworkLink::transmitCommand(uint8_t targetNode, float angleDeg) {
  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_CMD;
  pkt.targetNode = targetNode;
  pkt.angleDeg = angleDeg;
  pkt.sessionId = sessionId_;
  pkt.seq = nextSeq_++;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

bool NetworkLink::sendCommand(uint8_t targetNode, float angleDeg) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;
  const bool ok = transmitCommand(targetNode, angleDeg);
  if (ok) recordLastCommand(targetNode, angleDeg, millis());
  return ok;
}

void NetworkLink::recordLastCommand(uint8_t targetNode, float angleDeg, uint32_t now) {
  int freeSlot = -1;
  for (uint8_t i = 0; i < NET_MAX_LAST_COMMANDS; i++) {
    if (lastCommands_[i].inUse && lastCommands_[i].targetNode == targetNode) {
      lastCommands_[i].angleDeg = angleDeg;
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
  lastCommands_[slot].angleDeg = angleDeg;
  lastCommands_[slot].lastSentMs = now;
  lastCommands_[slot].inUse = true;
}

void NetworkLink::resendDueCommands(uint32_t now) {
  for (uint8_t i = 0; i < NET_MAX_LAST_COMMANDS; i++) {
    LastCommand &cmd = lastCommands_[i];
    if (!cmd.inUse) continue;
    if (now - cmd.lastSentMs >= NET_CMD_RESEND_INTERVAL_MS) {
      transmitCommand(cmd.targetNode, cmd.angleDeg);
      cmd.lastSentMs = now;
    }
  }
}

void NetworkLink::recordHello(uint8_t fromNodeId, float angleDeg, uint32_t now) {
  int freeSlot = -1;
  for (uint8_t i = 0; i < NET_MAX_TRACKED_NODES; i++) {
    if (knownNodes_[i].inUse && knownNodes_[i].id == fromNodeId) {
      knownNodes_[i].angleDeg = angleDeg;
      knownNodes_[i].lastSeenMs = now;
      return;
    }
    if (freeSlot < 0 && !knownNodes_[i].inUse) freeSlot = i;
  }
  if (freeSlot >= 0) {
    KnownNode &slot = knownNodes_[freeSlot];
    slot.id = fromNodeId;
    slot.angleDeg = angleDeg;
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
    recordHello(pkt.nodeId, pkt.angleDeg, now);
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_CMD) {
    if (pkt.targetNode != NET_BROADCAST_NODE && pkt.targetNode != nodeId_) return;
    if (!isfinite(pkt.angleDeg)) return; // corrupted/garbage payload: never let this reach the servo

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
    if (nodeCommandCb_) nodeCommandCb_(pkt.angleDeg);
  }
}

#include "NetworkLink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

namespace {
// esp_now_register_recv_cb takes a plain function pointer, so a single
// static instance pointer bridges the C callback back into the object.
// Fine in practice: exactly one NetworkLink is ever constructed (in main.cpp).
NetworkLink *g_instance = nullptr;

void onRecvTrampoline(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
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
  Serial.printf("[NET] esp-now ready, role=%s node_id=%u\n",
                mode_ == OperatingMode::MASTER ? "master" : "node", nodeId_);
}

void NetworkLink::loopTick(uint32_t now) {
  if (!espNowReady_ || mode_ != OperatingMode::NODE) return;

  if (now - lastHelloMs_ >= NET_HELLO_INTERVAL_MS) {
    lastHelloMs_ = now;
    NetPacket pkt;
    pkt.type = NET_PACKET_TYPE_HELLO;
    pkt.nodeId = nodeId_;
    pkt.angleDeg = localAngle_;
    esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt));
  }
}

bool NetworkLink::sendCommand(uint8_t targetNode, float angleDeg) {
  if (!espNowReady_ || mode_ != OperatingMode::MASTER) return false;

  NetPacket pkt;
  pkt.type = NET_PACKET_TYPE_CMD;
  pkt.targetNode = targetNode;
  pkt.angleDeg = angleDeg;
  return esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
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
    knownNodes_[freeSlot] = KnownNode{fromNodeId, angleDeg, now, true};
  }
}

void NetworkLink::onRecv(const uint8_t *data, int len) {
  if (len != sizeof(NetPacket)) return;
  NetPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.magic != NET_PACKET_MAGIC || pkt.version != NET_PACKET_VERSION) return;

  const uint32_t now = millis();

  if (mode_ == OperatingMode::MASTER && pkt.type == NET_PACKET_TYPE_HELLO) {
    recordHello(pkt.nodeId, pkt.angleDeg, now);
  } else if (mode_ == OperatingMode::NODE && pkt.type == NET_PACKET_TYPE_CMD) {
    if (pkt.targetNode == NET_BROADCAST_NODE || pkt.targetNode == nodeId_) {
      if (nodeCommandCb_) nodeCommandCb_(pkt.angleDeg);
    }
  }
}

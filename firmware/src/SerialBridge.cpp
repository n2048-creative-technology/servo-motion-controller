#include "SerialBridge.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>

#include "Config.h"
#include "NetworkLink.h"

void SerialBridge::begin(NetworkLink *network) {
  network_ = network;
  lineLen_ = 0;
}

void SerialBridge::loopTick() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\n') {
      lineBuf_[lineLen_] = '\0';
      handleLine(lineBuf_);
      lineLen_ = 0;
    } else if (c != '\r' && lineLen_ < kLineBufSize - 1) {
      lineBuf_[lineLen_++] = c;
    }
    // Silently drops overlong lines' excess bytes rather than growing the
    // buffer; a well-formed command line never approaches kLineBufSize.
  }
}

void SerialBridge::handleLine(const char *line) {
  if (line[0] == '\0') return;
  if (network_ == nullptr) {
    Serial.println("{\"ok\":false,\"error\":\"not_master\"}");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.println("{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }

  const char *cmd = doc["cmd"] | "";
  if (strcmp(cmd, "remote_record_start") == 0) {
    if (!doc["node"].is<int>()) {
      Serial.println("{\"ok\":false,\"error\":\"expected node\"}");
      return;
    }
    int node = doc["node"].as<int>();
    if (node < 0 || node > NET_NODE_ID_MAX) {
      Serial.println("{\"ok\":false,\"error\":\"node out of range\"}");
      return;
    }
    bool ok = network_->sendSeqStart(static_cast<uint8_t>(node));
    Serial.println(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
    return;
  }

  if (strcmp(cmd, "remote_record_stop") == 0) {
    if (!doc["node"].is<int>() || !doc["name"].is<const char *>()) {
      Serial.println("{\"ok\":false,\"error\":\"expected node+name\"}");
      return;
    }
    int node = doc["node"].as<int>();
    if (node < 0 || node > NET_NODE_ID_MAX) {
      Serial.println("{\"ok\":false,\"error\":\"node out of range\"}");
      return;
    }
    bool ok = network_->sendSeqStop(static_cast<uint8_t>(node), doc["name"].as<const char *>());
    Serial.println(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
    return;
  }

  if (strcmp(cmd, "remote_clear") == 0) {
    if (!doc["node"].is<int>()) {
      Serial.println("{\"ok\":false,\"error\":\"expected node\"}");
      return;
    }
    int node = doc["node"].as<int>();
    if (node < 0 || node > NET_NODE_ID_MAX) {
      Serial.println("{\"ok\":false,\"error\":\"node out of range\"}");
      return;
    }
    bool ok = network_->sendSeqClear(static_cast<uint8_t>(node));
    Serial.println(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
    return;
  }

  if (strcmp(cmd, "space_query") == 0) {
    if (!doc["node"].is<int>()) {
      Serial.println("{\"ok\":false,\"error\":\"expected node\"}");
      return;
    }
    int node = doc["node"].as<int>();
    if (node < 0 || node > NET_NODE_ID_MAX) {
      Serial.println("{\"ok\":false,\"error\":\"node out of range\"}");
      return;
    }
    bool ok = network_->sendSpaceQuery(static_cast<uint8_t>(node));
    Serial.println(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
    return;
  }

  if (strcmp(cmd, "list") == 0) {
    JsonDocument out;
    out["type"] = "nodes";
    JsonArray nodes = out["nodes"].to<JsonArray>();
    const KnownNode *known = network_->knownNodes();
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NetworkLink::maxKnownNodes(); i++) {
      if (!known[i].inUse) continue;
      JsonObject n = nodes.add<JsonObject>();
      n["id"] = known[i].id;
      n["angle"] = known[i].angleDeg;
      n["age_ms"] = now - known[i].lastSeenMs;
    }
    String outStr;
    serializeJson(out, outStr);
    Serial.println(outStr);
    return;
  }

  if (!doc["node"].is<int>() || !doc["angle"].is<float>()) {
    Serial.println("{\"ok\":false,\"error\":\"expected node+angle or cmd\"}");
    return;
  }

  int node = doc["node"].as<int>();
  float angle = doc["angle"].as<float>();
  if (node < 0 || node > NET_NODE_ID_MAX) {
    Serial.println("{\"ok\":false,\"error\":\"node out of range\"}");
    return;
  }

  bool ok = network_->sendCommand(static_cast<uint8_t>(node), angle);
  Serial.println(ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"send_failed\"}");
}

namespace {
const char *seqAckStatusReason(SeqAckStatus status) {
  switch (status) {
    case SeqAckStatus::Ok: return "";
    case SeqAckStatus::NoPointsCaptured:
      return "Node captured no movement — its start request likely never arrived, or it lost "
             "power/reset partway through the transfer";
    case SeqAckStatus::InvalidName: return "sequence name was invalid after sanitizing";
    case SeqAckStatus::WriteFailed: return "Node failed to write the file (its flash may be full)";
    default: return "unknown failure";
  }
}
} // namespace

void SerialBridge::reportUploadResult(uint8_t nodeId, const char *name, SeqAckStatus status, uint16_t points) {
  JsonDocument out;
  out["type"] = "upload_result";
  out["node"] = nodeId;
  out["name"] = name;
  out["ok"] = status == SeqAckStatus::Ok;
  out["points"] = points;
  if (status != SeqAckStatus::Ok) out["reason"] = seqAckStatusReason(status);
  String outStr;
  serializeJson(out, outStr);
  Serial.println(outStr);
}

void SerialBridge::reportSpaceReply(uint8_t nodeId, uint32_t freeBytes) {
  JsonDocument out;
  out["type"] = "space_reply";
  out["node"] = nodeId;
  out["free_bytes"] = freeBytes;
  String outStr;
  serializeJson(out, outStr);
  Serial.println(outStr);
}

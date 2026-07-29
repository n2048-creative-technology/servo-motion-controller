#include "WebApi.h"

#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <vector>

#include "PlaybackEngine.h"
#include "SequenceStore.h"
#include "SettingsStore.h"
#include "ServoController.h"
#include "NetworkLink.h"
#include "IAngleSink.h"
#include "Config.h"

namespace {

const char *modeToString(PlaybackMode mode) {
  switch (mode) {
    case PlaybackMode::IDLE: return "idle";
    case PlaybackMode::MANUAL: return "manual";
    case PlaybackMode::RECORDING: return "recording";
    case PlaybackMode::PATTERN: return "pattern";
    case PlaybackMode::SEQUENCE: return "sequence";
    case PlaybackMode::NETWORK: return "network";
  }
  return "idle";
}

const char *networkModeToString(OperatingMode mode) {
  switch (mode) {
    case OperatingMode::STANDALONE: return "standalone";
    case OperatingMode::NODE: return "node";
    case OperatingMode::MASTER: return "master";
  }
  return "standalone";
}

OperatingMode networkModeFromString(const char *s) {
  if (!s) return OperatingMode::STANDALONE;
  if (strcmp(s, "node") == 0) return OperatingMode::NODE;
  if (strcmp(s, "master") == 0) return OperatingMode::MASTER;
  return OperatingMode::STANDALONE;
}

const char *patternTypeToString(PatternType type) {
  switch (type) {
    case PatternType::SINE: return "sine";
    case PatternType::SQUARE: return "square";
    case PatternType::TRIANGLE: return "triangle";
    case PatternType::SAWTOOTH: return "sawtooth";
    case PatternType::TRAPEZOID: return "trapezoid";
  }
  return "sine";
}

PatternType patternTypeFromString(const char *s) {
  if (!s) return PatternType::SINE;
  if (strcmp(s, "square") == 0) return PatternType::SQUARE;
  if (strcmp(s, "triangle") == 0) return PatternType::TRIANGLE;
  if (strcmp(s, "sawtooth") == 0) return PatternType::SAWTOOTH;
  if (strcmp(s, "trapezoid") == 0) return PatternType::TRAPEZOID;
  return PatternType::SINE;
}

PatternParams parsePatternParams(JsonVariant json, const PatternParams &fallback) {
  PatternParams p = fallback;
  if (json["type"].is<const char *>()) p.type = patternTypeFromString(json["type"]);
  if (json["period_ms"].is<uint32_t>()) p.periodMs = json["period_ms"].as<uint32_t>();
  if (json["amplitude_deg"].is<float>()) p.amplitudeDeg = json["amplitude_deg"].as<float>();
  if (json["offset_deg"].is<float>()) p.offsetDeg = json["offset_deg"].as<float>();
  if (json["duty_pct"].is<float>()) p.dutyPct = json["duty_pct"].as<float>();
  if (json["rise_pct"].is<float>()) p.risePct = json["rise_pct"].as<float>();
  if (json["hold_pct"].is<float>()) p.holdPct = json["hold_pct"].as<float>();
  if (json["fall_pct"].is<float>()) p.fallPct = json["fall_pct"].as<float>();
  return p;
}

void writePatternParams(JsonObject obj, const PatternParams &p) {
  obj["type"] = patternTypeToString(p.type);
  obj["period_ms"] = p.periodMs;
  obj["amplitude_deg"] = p.amplitudeDeg;
  obj["offset_deg"] = p.offsetDeg;
  obj["duty_pct"] = p.dutyPct;
  obj["rise_pct"] = p.risePct;
  obj["hold_pct"] = p.holdPct;
  obj["fall_pct"] = p.fallPct;
}

} // namespace

void WebApi::begin(PlaybackEngine *playback, SequenceStore *sequence, SettingsStore *settingsStore,
                    ServoController *servo, NetworkLink *network, IAngleSink *angleSink,
                    const IPAddress &apIp) {
  playback_ = playback;
  sequence_ = sequence;
  settingsStore_ = settingsStore;
  servo_ = servo;
  network_ = network;
  angleSink_ = angleSink;

  dns_.start(DNS_PORT, "*", apIp);

  setupRoutes();

  ws_.onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                      uint8_t *data, size_t len) { onWsEvent(server, client, type, arg, data, len); });
  server_.addHandler(&ws_);

  server_.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // Best-effort captive portal: any unmatched path gets redirected to the app,
  // which is enough to trigger the "sign in to network" prompt on most phones.
  // (Reliable auto-popup across every OS isn't achievable with a DNS catch-all
  // alone; the fixed fallback IP is documented in the README.)
  server_.onNotFound([apIp](AsyncWebServerRequest *request) {
    String url = "http://" + apIp.toString() + "/";
    request->redirect(url);
  });

  server_.begin();
}

void WebApi::loopTick(uint32_t now) {
  dns_.processNextRequest();

  if (now - lastStatusBroadcastMs_ >= STATUS_BROADCAST_MS) {
    lastStatusBroadcastMs_ = now;
    broadcastStatus();
    ws_.cleanupClients();
  }
}

String WebApi::buildStatusJson() {
  JsonDocument doc;
  doc["type"] = "status";
  doc["mode"] = modeToString(playback_->mode());
  doc["angle"] = angleSink_->getAngle();
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["firmware_version"] = FIRMWARE_VERSION;

  JsonObject recording = doc["recording"].to<JsonObject>();
  recording["active"] = playback_->mode() == PlaybackMode::RECORDING;
  recording["points"] = sequence_->recordedPointCount();

  JsonObject sequenceObj = doc["sequence"].to<JsonObject>();
  sequenceObj["present"] = sequence_->hasSequence();
  sequenceObj["points"] = sequence_->pointCount();
  sequenceObj["duration_ms"] = sequence_->durationMs();

  String out;
  serializeJson(doc, out);
  return out;
}

void WebApi::broadcastStatus() {
  if (ws_.count() == 0) return;
  ws_.textAll(buildStatusJson());
}

void WebApi::onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                        uint8_t *data, size_t len) {
  (void)server;
  (void)client;
  if (type != WS_EVT_DATA) return;

  AwsFrameInfo *info = static_cast<AwsFrameInfo *>(arg);
  if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) return;

  const char *cmd = doc["cmd"] | "";
  if (strcmp(cmd, "jog") == 0 && doc["angle"].is<float>()) {
    playback_->onJog(doc["angle"].as<float>(), millis());
  }
}

void WebApi::setupRoutes() {
  server_.on("/api/status", HTTP_GET, [this](AsyncWebServerRequest *request) {
    request->send(200, "application/json", buildStatusJson());
  });

  server_.on("/api/patterns", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    struct Entry {
      const char *type;
      const char *label;
      const char *params;
    };
    static const Entry entries[] = {
        {"sine", "Sine", "period_ms,amplitude_deg,offset_deg"},
        {"square", "Square", "period_ms,amplitude_deg,offset_deg,duty_pct"},
        {"triangle", "Triangle", "period_ms,amplitude_deg,offset_deg"},
        {"sawtooth", "Sawtooth", "period_ms,amplitude_deg,offset_deg"},
        {"trapezoid", "Trapezoid", "period_ms,amplitude_deg,offset_deg,rise_pct,hold_pct,fall_pct"},
    };
    for (const auto &e : entries) {
      JsonObject obj = arr.add<JsonObject>();
      obj["type"] = e.type;
      obj["label"] = e.label;
      obj["params"] = e.params;
    }

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/pattern/start", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        PatternParams p = parsePatternParams(json, playback_->activePattern());
        playback_->startPattern(p, millis());
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/pattern/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->stopPattern();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/manual/jog", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (json["angle_deg"].is<float>()) {
          playback_->onJog(json["angle_deg"].as<float>(), millis());
        }
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/record/start", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->startRecording(millis());
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/record/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->stopRecording();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/record/save", HTTP_POST, [this](AsyncWebServerRequest *request) {
    bool ok = sequence_->saveToFS();
    request->send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
  });

  server_.on("/api/record/discard", HTTP_POST, [this](AsyncWebServerRequest *request) {
    sequence_->discardRecording();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/sequence", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["present"] = sequence_->hasSequence();
    doc["points"] = sequence_->pointCount();
    doc["duration_ms"] = sequence_->durationMs();
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server_.on("/api/sequence/play", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->startSequencePlayback(millis());
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/sequence/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->stopSequencePlayback();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) {
    const PersistedSettings &s = settingsStore_->settings();
    JsonDocument doc;

    JsonObject ap = doc["ap"].to<JsonObject>();
    ap["ssid"] = s.apSsid;
    ap["has_password"] = strlen(s.apPassword) > 0;

    JsonObject servoObj = doc["servo"].to<JsonObject>();
    servoObj["min_us"] = s.servoMinUs;
    servoObj["max_us"] = s.servoMaxUs;
    servoObj["min_angle"] = s.servoMinAngle;
    servoObj["max_angle"] = s.servoMaxAngle;
    servoObj["center_angle"] = s.servoCenterAngle;
    servoObj["invert"] = s.servoInvert;

    JsonObject autostart = doc["autostart"].to<JsonObject>();
    autostart["enabled"] = s.autostartEnabled;
    autostart["target"] = s.autostartTarget == AutostartTarget::PATTERN   ? "pattern"
                           : s.autostartTarget == AutostartTarget::SEQUENCE ? "sequence"
                                                                             : "none";
    writePatternParams(autostart["pattern"].to<JsonObject>(), s.autostartPattern);

    JsonObject network = doc["network"].to<JsonObject>();
    network["mode"] = networkModeToString(s.networkMode);
    network["node_id"] = s.nodeId;

    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/settings", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        PersistedSettings &s = settingsStore_->settings();

        if (json["ap"]["ssid"].is<const char *>()) {
          strncpy(s.apSsid, json["ap"]["ssid"].as<const char *>(), sizeof(s.apSsid) - 1);
        }
        if (json["ap"]["password"].is<const char *>()) {
          const char *pw = json["ap"]["password"].as<const char *>();
          if (strlen(pw) == 0 || strlen(pw) >= 8) {
            strncpy(s.apPassword, pw, sizeof(s.apPassword) - 1);
          }
        }

        bool calibrationChanged = false;
        if (json["servo"]["min_us"].is<uint16_t>()) {
          s.servoMinUs = json["servo"]["min_us"].as<uint16_t>();
          calibrationChanged = true;
        }
        if (json["servo"]["max_us"].is<uint16_t>()) {
          s.servoMaxUs = json["servo"]["max_us"].as<uint16_t>();
          calibrationChanged = true;
        }
        if (json["servo"]["min_angle"].is<float>()) {
          s.servoMinAngle = json["servo"]["min_angle"].as<float>();
          calibrationChanged = true;
        }
        if (json["servo"]["max_angle"].is<float>()) {
          s.servoMaxAngle = json["servo"]["max_angle"].as<float>();
          calibrationChanged = true;
        }
        if (json["servo"]["center_angle"].is<float>()) {
          s.servoCenterAngle = json["servo"]["center_angle"].as<float>();
        }
        if (calibrationChanged) {
          servo_->setCalibration(s.servoMinUs, s.servoMaxUs, s.servoMinAngle, s.servoMaxAngle);
        }
        if (json["servo"]["invert"].is<bool>()) {
          s.servoInvert = json["servo"]["invert"].as<bool>();
          servo_->setInvert(s.servoInvert);
        }

        if (json["autostart"]["enabled"].is<bool>()) {
          s.autostartEnabled = json["autostart"]["enabled"].as<bool>();
        }
        if (json["autostart"]["target"].is<const char *>()) {
          const char *t = json["autostart"]["target"];
          s.autostartTarget = strcmp(t, "pattern") == 0   ? AutostartTarget::PATTERN
                               : strcmp(t, "sequence") == 0 ? AutostartTarget::SEQUENCE
                                                             : AutostartTarget::NONE;
        }
        if (json["autostart"]["pattern"].is<JsonObject>()) {
          s.autostartPattern = parsePatternParams(json["autostart"]["pattern"], s.autostartPattern);
        }

        if (json["network"]["mode"].is<const char *>()) {
          s.networkMode = networkModeFromString(json["network"]["mode"].as<const char *>());
        }
        if (json["network"]["node_id"].is<uint8_t>()) {
          uint8_t id = json["network"]["node_id"].as<uint8_t>();
          if (id >= NET_NODE_ID_MIN && id <= NET_NODE_ID_MAX) {
            s.nodeId = id;
          }
        }

        settingsStore_->save();
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/network/nodes", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray nodes = doc["nodes"].to<JsonArray>();
    const KnownNode *known = network_->knownNodes();
    const uint32_t now = millis();
    for (uint8_t i = 0; i < NetworkLink::maxKnownNodes(); i++) {
      if (!known[i].inUse) continue;
      JsonObject n = nodes.add<JsonObject>();
      n["id"] = known[i].id;
      n["angle"] = known[i].angleDeg;
      n["age_ms"] = now - known[i].lastSeenMs;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server_.on("/api/network/targets", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["broadcast_all"] = angleSink_->targetsBroadcastAll();
    JsonArray ids = doc["node_ids"].to<JsonArray>();
    for (size_t i = 0; i < angleSink_->targetCount(); i++) ids.add(angleSink_->targetAt(i));
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Master only in practice (a no-op on Standalone/Node boards, whose sink
  // ignores setTargets): picks which Node(s) the Manual tab's jog/pattern
  // controls currently drive — broadcast to all, or an explicit id list.
  // Ephemeral (RAM only), not persisted to NVS — resets to "all nodes" on
  // reboot.
  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/network/targets", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        bool broadcastAll = json["broadcast_all"] | true;
        std::vector<uint8_t> ids;
        if (json["node_ids"].is<JsonArray>()) {
          for (JsonVariant v : json["node_ids"].as<JsonArray>()) {
            int id = v.as<int>();
            if (id >= 0 && id <= NET_NODE_ID_MAX) ids.push_back(static_cast<uint8_t>(id));
          }
        }
        angleSink_->setTargets(broadcastAll, ids.data(), ids.size());
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/settings/reset", HTTP_POST, [this](AsyncWebServerRequest *request) {
    settingsStore_->factoryDefaults();
    settingsStore_->save();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.on("/api/reboot", HTTP_POST, [this](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    // Give the response a moment to flush before resetting.
    delay(200);
    ESP.restart();
  });
}

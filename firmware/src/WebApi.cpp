#include "WebApi.h"

#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <vector>

#include "PlaybackEngine.h"
#include "SequenceStore.h"
#include "SettingsStore.h"
#include "ServoPair.h"
#include "RelayController.h"
#include "NetworkLink.h"
#include "IMotionSink.h"
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
    case PatternType::RANDOM: return "random";
  }
  return "sine";
}

PatternType patternTypeFromString(const char *s) {
  if (!s) return PatternType::SINE;
  if (strcmp(s, "square") == 0) return PatternType::SQUARE;
  if (strcmp(s, "triangle") == 0) return PatternType::TRIANGLE;
  if (strcmp(s, "sawtooth") == 0) return PatternType::SAWTOOTH;
  if (strcmp(s, "trapezoid") == 0) return PatternType::TRAPEZOID;
  if (strcmp(s, "random") == 0) return PatternType::RANDOM;
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
  // Taken as floats and rounded rather than required to be integral: a UI
  // that hands back "1500.0" for a millisecond field shouldn't have its value
  // silently ignored.
  if (json["interval_min_ms"].is<float>()) {
    p.randMinIntervalMs = static_cast<uint32_t>(clampValue(json["interval_min_ms"].as<float>(), 0.0f, 600000.0f));
  }
  if (json["interval_max_ms"].is<float>()) {
    p.randMaxIntervalMs = static_cast<uint32_t>(clampValue(json["interval_max_ms"].as<float>(), 0.0f, 600000.0f));
  }
  if (json["max_speed_dps"].is<float>()) {
    p.randMaxSpeedDps = clampValue(json["max_speed_dps"].as<float>(), PATTERN_RANDOM_MIN_SPEED_DPS,
                                    PATTERN_RANDOM_MAX_SPEED_DPS);
  }
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
  obj["interval_min_ms"] = p.randMinIntervalMs;
  obj["interval_max_ms"] = p.randMaxIntervalMs;
  obj["max_speed_dps"] = p.randMaxSpeedDps;
}

void writeServoCalibration(JsonObject obj, const ServoCalibration &c, uint8_t pin) {
  obj["pin"] = pin;
  obj["min_us"] = c.minUs;
  obj["max_us"] = c.maxUs;
  obj["min_angle"] = c.minAngle;
  obj["max_angle"] = c.maxAngle;
  obj["center_angle"] = c.centerAngle;
  obj["invert"] = c.invert;
}

// Applies whichever fields are present to one axis, pushing them straight to
// that servo. Returns true if anything that affects travel changed, so the
// caller can re-derive the pattern limits once for both axes.
bool applyServoCalibration(JsonVariant json, ServoCalibration &c, ServoController &servo) {
  if (!json.is<JsonObject>()) return false;
  bool changed = false;
  if (json["min_us"].is<uint16_t>()) { c.minUs = json["min_us"].as<uint16_t>(); changed = true; }
  if (json["max_us"].is<uint16_t>()) { c.maxUs = json["max_us"].as<uint16_t>(); changed = true; }
  if (json["min_angle"].is<float>()) { c.minAngle = json["min_angle"].as<float>(); changed = true; }
  if (json["max_angle"].is<float>()) { c.maxAngle = json["max_angle"].as<float>(); changed = true; }
  if (json["center_angle"].is<float>()) c.centerAngle = json["center_angle"].as<float>();
  if (changed) servo.setCalibration(c.minUs, c.maxUs, c.minAngle, c.maxAngle);
  if (json["invert"].is<bool>()) {
    c.invert = json["invert"].as<bool>();
    servo.setInvert(c.invert);
  }
  return changed;
}

} // namespace

void WebApi::begin(PlaybackEngine *playback, SequenceStore *sequence, SettingsStore *settingsStore,
                    ServoPair *servos, RelayController *relay, NetworkLink *network,
                    IMotionSink *motionSink, const IPAddress &apIp) {
  playback_ = playback;
  sequence_ = sequence;
  settingsStore_ = settingsStore;
  servos_ = servos;
  relay_ = relay;
  network_ = network;
  motionSink_ = motionSink;

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
  doc["x"] = motionSink_->getX();
  doc["y"] = motionSink_->getY();
  // Deprecated alias for X, kept so anything written against the
  // single-servo API keeps reading a sensible value.
  doc["angle"] = motionSink_->getX();
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["relay_on"] = playback_->relayState();

  JsonObject recording = doc["recording"].to<JsonObject>();
  recording["active"] = playback_->mode() == PlaybackMode::RECORDING;
  recording["points"] = sequence_->recordedPointCount();

  JsonObject sequenceObj = doc["sequence"].to<JsonObject>();
  sequenceObj["present"] = sequence_->hasSequence();
  sequenceObj["name"] = sequence_->activeName();
  sequenceObj["points"] = sequence_->pointCount();
  sequenceObj["duration_ms"] = sequence_->durationMs();
  // Drives the web UI's playhead. Only meaningful while a sequence is
  // actually playing; 0 otherwise.
  sequenceObj["playing"] = playback_->mode() == PlaybackMode::SEQUENCE;
  sequenceObj["position_ms"] = playback_->sequencePositionMs(millis());

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
  if (strcmp(cmd, "jog") == 0) {
    // The trackpad sends both axes together; "angle" is still read as X for
    // anything written against the single-servo API. An axis that isn't sent
    // holds its current position rather than snapping to a default.
    const float x = doc["x"].is<float>()       ? doc["x"].as<float>()
                    : doc["angle"].is<float>() ? doc["angle"].as<float>()
                                               : motionSink_->getX();
    const float y = doc["y"].is<float>() ? doc["y"].as<float>() : motionSink_->getY();
    if (doc["x"].is<float>() || doc["y"].is<float>() || doc["angle"].is<float>()) {
      playback_->onJog(x, y, millis());
    }
  } else if (strcmp(cmd, "relay") == 0 && doc["on"].is<bool>()) {
    playback_->onRelayToggle(doc["on"].as<bool>());
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
        // No amplitude/offset: RANDOM sweeps the servo's whole calibrated
        // range (Settings → Servo Calibration) rather than a window inside it.
        {"random", "Random", "interval_min_ms,interval_max_ms,max_speed_dps"},
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
        // {"x": {...}, "y": {...}} — either may be omitted to leave that axis
        // on whatever it was last given. A flat body (no x/y objects) is read
        // as the X axis, so a single-servo-era caller still does something
        // sensible instead of nothing.
        PatternParams px = playback_->activePatternX();
        PatternParams py = playback_->activePatternY();
        if (json["x"].is<JsonObject>() || json["y"].is<JsonObject>()) {
          if (json["x"].is<JsonObject>()) px = parsePatternParams(json["x"], px);
          if (json["y"].is<JsonObject>()) py = parsePatternParams(json["y"], py);
        } else {
          px = parsePatternParams(json, px);
        }
        playback_->startPattern(px, py, millis());
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/pattern/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->stopPattern();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/manual/jog", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        const bool hasX = json["x_deg"].is<float>() || json["angle_deg"].is<float>();
        const bool hasY = json["y_deg"].is<float>();
        if (hasX || hasY) {
          const float x = json["x_deg"].is<float>()       ? json["x_deg"].as<float>()
                          : json["angle_deg"].is<float>() ? json["angle_deg"].as<float>()
                                                          : motionSink_->getX();
          const float y = hasY ? json["y_deg"].as<float>() : motionSink_->getY();
          playback_->onJog(x, y, millis());
        }
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  // The light next to the jog fader. Deliberately doesn't change playback
  // mode: switching it mid-pattern shouldn't stop the pattern, and mid-
  // recording it's captured by the ordinary capture tick alongside the angle.
  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/relay", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json["on"].is<bool>()) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"expected on\"}");
          return;
        }
        playback_->onRelayToggle(json["on"].as<bool>());
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

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/record/save", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json["name"].is<const char *>()) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"expected name\"}");
          return;
        }
        bool ok = sequence_->saveAs(json["name"].as<const char *>()) == SaveResult::Ok;
        request->send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
      }));

  server_.on("/api/record/discard", HTTP_POST, [this](AsyncWebServerRequest *request) {
    sequence_->discardRecording();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // Every saved sequence on this board (locally recorded or uploaded via a
  // Master) — name, size, duration. Drives the Record tab's list and the
  // Autostart target=sequence picker in the web UI.
  server_.on("/api/sequences", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    SequenceInfo infos[SEQ_MAX_LISTED];
    uint8_t n = sequence_->listSequences(infos, SEQ_MAX_LISTED);
    for (uint8_t i = 0; i < n; i++) {
      JsonObject obj = arr.add<JsonObject>();
      obj["name"] = infos[i].name;
      obj["points"] = infos[i].points;
      obj["duration_ms"] = infos[i].durationMs;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // A downsampled view of one saved sequence, for plotting it in the Record
  // tab. Reading a sequence never disturbs whatever is currently playing.
  server_.on("/api/sequence/data", HTTP_GET, [this](AsyncWebServerRequest *request) {
    if (!request->hasParam("name")) {
      request->send(400, "application/json", "{\"ok\":false,\"error\":\"expected name\"}");
      return;
    }
    uint16_t maxPoints = SEQ_PLOT_MAX_POINTS;
    if (request->hasParam("points")) {
      const long asked = request->getParam("points")->value().toInt();
      if (asked > 0) maxPoints = clampValue<uint16_t>(static_cast<uint16_t>(asked), 2, SEQ_PLOT_MAX_POINTS);
    }

    String out;
    // Roughly what one point costs as JSON, so the string doesn't spend the
    // whole build reallocating.
    out.reserve(64 + maxPoints * 28);
    if (!sequence_->appendPlotJson(request->getParam("name")->value().c_str(), maxPoints, out)) {
      request->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown sequence\"}");
      return;
    }
    request->send(200, "application/json", out);
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/sequence/play", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json["name"].is<const char *>() || !sequence_->loadNamed(json["name"].as<const char *>())) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown sequence\"}");
          return;
        }
        playback_->startSequencePlayback(millis());
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/sequence/stop", HTTP_POST, [this](AsyncWebServerRequest *request) {
    playback_->stopSequencePlayback();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server_.addHandler(new AsyncCallbackJsonWebHandler(
      "/api/sequence/delete", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!json["name"].is<const char *>() || !sequence_->deleteSequence(json["name"].as<const char *>())) {
          request->send(400, "application/json", "{\"ok\":false,\"error\":\"delete failed\"}");
          return;
        }
        request->send(200, "application/json", "{\"ok\":true}");
      }));

  server_.on("/api/sequences/clear", HTTP_POST, [this](AsyncWebServerRequest *request) {
    uint8_t removed = sequence_->clearAll();
    String out = "{\"ok\":true,\"removed\":" + String(removed) + "}";
    request->send(200, "application/json", out);
  });

  server_.on("/api/settings", HTTP_GET, [this](AsyncWebServerRequest *request) {
    const PersistedSettings &s = settingsStore_->settings();
    JsonDocument doc;

    JsonObject ap = doc["ap"].to<JsonObject>();
    ap["ssid"] = s.apSsid;
    ap["has_password"] = strlen(s.apPassword) > 0;

    JsonObject servoObj = doc["servo"].to<JsonObject>();
    writeServoCalibration(servoObj["x"].to<JsonObject>(), s.servoX, SERVO_X_PIN);
    writeServoCalibration(servoObj["y"].to<JsonObject>(), s.servoY, SERVO_Y_PIN);

    JsonObject relayObj = doc["relay"].to<JsonObject>();
    relayObj["pin"] = relay_ ? relay_->pin() : RELAY_PIN;
    relayObj["active_low"] = s.relayActiveLow;
    relayObj["on"] = playback_->relayState();

    JsonObject autostart = doc["autostart"].to<JsonObject>();
    autostart["enabled"] = s.autostartEnabled;
    autostart["target"] = s.autostartTarget == AutostartTarget::PATTERN   ? "pattern"
                           : s.autostartTarget == AutostartTarget::SEQUENCE ? "sequence"
                                                                             : "none";
    JsonObject autoPattern = autostart["pattern"].to<JsonObject>();
    writePatternParams(autoPattern["x"].to<JsonObject>(), s.autostartPatternX);
    writePatternParams(autoPattern["y"].to<JsonObject>(), s.autostartPatternY);
    autostart["sequence_name"] = s.autostartSequenceName;

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

        // Each axis is applied independently, so saving one doesn't disturb
        // the other.
        bool calibrationChanged = false;
        calibrationChanged |= applyServoCalibration(json["servo"]["x"], s.servoX, servos_->x());
        calibrationChanged |= applyServoCalibration(json["servo"]["y"], s.servoY, servos_->y());
        if (calibrationChanged) {
          // Keeps the RANDOM pattern's targets inside the new travel limits
          // without waiting for a reboot.
          playback_->setAngleLimits(s.servoX.minAngle, s.servoX.maxAngle, s.servoY.minAngle,
                                    s.servoY.maxAngle);
        }

        if (json["relay"]["active_low"].is<bool>()) {
          s.relayActiveLow = json["relay"]["active_low"].as<bool>();
          if (relay_) relay_->setActiveLow(s.relayActiveLow);
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
        if (json["autostart"]["pattern"]["x"].is<JsonObject>()) {
          s.autostartPatternX = parsePatternParams(json["autostart"]["pattern"]["x"], s.autostartPatternX);
        }
        if (json["autostart"]["pattern"]["y"].is<JsonObject>()) {
          s.autostartPatternY = parsePatternParams(json["autostart"]["pattern"]["y"], s.autostartPatternY);
        }
        if (json["autostart"]["sequence_name"].is<const char *>()) {
          strncpy(s.autostartSequenceName, json["autostart"]["sequence_name"].as<const char *>(),
                   sizeof(s.autostartSequenceName) - 1);
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
      n["x"] = known[i].angleX;
      n["y"] = known[i].angleY;
      n["relay"] = known[i].relayOn;
      n["age_ms"] = now - known[i].lastSeenMs;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server_.on("/api/network/targets", HTTP_GET, [this](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["broadcast_all"] = motionSink_->targetsBroadcastAll();
    JsonArray ids = doc["node_ids"].to<JsonArray>();
    for (size_t i = 0; i < motionSink_->targetCount(); i++) ids.add(motionSink_->targetAt(i));
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
        motionSink_->setTargets(broadcastAll, ids.data(), ids.size());
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

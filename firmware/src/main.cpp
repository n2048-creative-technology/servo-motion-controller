#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "Config.h"
#include "IMotionSink.h"
#include "IRelaySink.h"
#include "ServoPair.h"
#include "RelayController.h"
#include "SettingsStore.h"
#include "SequenceStore.h"
#include "PlaybackEngine.h"
#include "WebApi.h"
#include "NetworkLink.h"
#include "NetworkMotionSink.h"
#include "SerialBridge.h"

ServoPair servos;
RelayController relay;
SettingsStore settingsStore;
SequenceStore sequenceStore;
PlaybackEngine playback;
WebApi webApi;
NetworkLink networkLink;
NetworkMotionSink networkMotionSink;
SerialBridge serialBridge;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

uint32_t lastTickMs = 0;
bool nodeMotorActive = false; // NODE only — see NODE_WIFI_TX_POWER_* in Config.h

// Role wiring (networkLink.begin/serialBridge.begin/sink selection below)
// only happens once here in setup(), matching the documented "network.*
// changes need a reboot" behavior. POST /api/settings can flip
// settingsStore's live networkMode in RAM immediately, well before any
// reboot — loop() must keep gating on this boot-time snapshot rather than
// re-reading live settings, or it starts calling serialBridge.loopTick()
// against a SerialBridge whose network_ pointer was never set (begin()
// never ran for the new role), crashing on the first PC command.
OperatingMode bootNetworkMode = OperatingMode::STANDALONE;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[SELFTEST] booting servo-motion-controller");
  Serial.printf("[SELFTEST] firmware version=%s\n", FIRMWARE_VERSION);

  settingsStore.load();
  const PersistedSettings &settings = settingsStore.settings();
  bootNetworkMode = settings.networkMode;
  Serial.printf("[SELFTEST] settings loaded (version=%u, autostart=%d)\n", settings.version,
                settings.autostartEnabled ? 1 : 0);

  servos.x().begin(SERVO_X_PIN, settings.servoX.minUs, settings.servoX.maxUs, settings.servoX.minAngle,
                   settings.servoX.maxAngle, settings.servoX.invert);
  servos.y().begin(SERVO_Y_PIN, settings.servoY.minUs, settings.servoY.maxUs, settings.servoY.minAngle,
                   settings.servoY.maxAngle, settings.servoY.invert);
  servos.writeAngles(settings.servoX.centerAngle, settings.servoY.centerAngle);
  Serial.printf("[SELFTEST] servos attached X=pin%d %u-%uus, Y=pin%d %u-%uus\n", SERVO_X_PIN,
                settings.servoX.minUs, settings.servoX.maxUs, SERVO_Y_PIN, settings.servoY.minUs,
                settings.servoY.maxUs);

  relay.begin(RELAY_PIN, settings.relayActiveLow);
  Serial.printf("[SELFTEST] relay pin=%d active_%s, starting off\n", RELAY_PIN,
                settings.relayActiveLow ? "low" : "high");

  bool fsOk = LittleFS.begin(true);
  Serial.printf("[SELFTEST] littlefs mount %s\n", fsOk ? "OK" : "FAILED");

  sequenceStore.begin();
  SequenceInfo seqInfos[SEQ_MAX_LISTED];
  uint8_t seqCount = sequenceStore.listSequences(seqInfos, SEQ_MAX_LISTED);
  Serial.printf("[SELFTEST] sequences stored: %u\n", seqCount);

  // Keeps the WiFi driver's own AP config (SSID/password/etc, a separate
  // blob from this project's own "app" NVS namespace) purely in RAM instead
  // of also writing it to flash on every WiFi.softAP() call. Without this, a
  // stale copy from a *previous* boot's config can end up fought over with
  // what's explicitly set below right after — a documented arduino-esp32
  // gotcha, and confirmed live on this project's own ESP32-S3 hardware: a
  // correct password was rejected with wpa_supplicant reporting WRONG_KEY
  // even right after a full chip erase.
  WiFi.persistent(false);

  // softAP() must run before softAPConfig() — the reverse order is a known
  // arduino-esp32 footgun where the AP's security (the password) silently
  // doesn't take, even though softAP() itself reports success and the
  // network still shows as WPA2 in a scan. See README.md's "Access point"
  // section.
  WiFi.mode(WIFI_AP);
  const bool hasPassword = strlen(settings.apPassword) >= 8;
  const bool apOk = hasPassword ? WiFi.softAP(settings.apSsid, settings.apPassword, AP_WIFI_CHANNEL)
                                 : WiFi.softAP(settings.apSsid, nullptr, AP_WIFI_CHANNEL);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  Serial.printf("[SELFTEST] wifi AP %s ssid=%s ip=%s\n", apOk ? "up" : "FAILED", settings.apSsid,
                WiFi.softAPIP().toString().c_str());

  networkLink.begin(settings.networkMode, settings.nodeId);

  // MASTER drives selected Node(s) over ESP-NOW instead of a local servo, so
  // PlaybackEngine's jog/pattern/sequence logic is repointed at a
  // NetworkAngleSink; every other mode drives the physical ServoController
  // exactly as in v1. Autostart only makes sense with a real attached servo.
  IMotionSink *sink = &servos;
  IRelaySink *relaySink = &relay;
  if (settings.networkMode == OperatingMode::MASTER) {
    // Always full power — see WIFI_TX_POWER_MAX's comment in Config.h. Set
    // explicitly rather than just never lowering it, so this doesn't
    // silently depend on the core's own boot-time default matching.
    WiFi.setTxPower(static_cast<wifi_power_t>(WIFI_TX_POWER_MAX));
    networkMotionSink.begin(&networkLink);
    sink = &networkMotionSink;
    // A Master's own D7 stays idle: its light toggle drives the selected
    // Node(s)' relays over ESP-NOW, exactly like its trackpad does.
    relaySink = &networkMotionSink;
    serialBridge.begin(&networkLink);
    networkLink.onSeqAck([](uint8_t nodeId, const char *name, SeqAckStatus status, uint16_t points) {
      serialBridge.reportUploadResult(nodeId, name, status, points);
    });
    networkLink.onSpaceReply([](uint8_t nodeId, uint32_t freeBytes) {
      serialBridge.reportSpaceReply(nodeId, freeBytes);
    });
  } else if (settings.networkMode == OperatingMode::NODE) {
    networkLink.onNodeCommand([](float angleX, float angleY, bool relayOn) {
      playback.onNetworkCommand(angleX, angleY, relayOn, millis());
    });
    // A Master remotely uploading a sequence is just a remotely-triggered
    // recording: start/stop reuse the exact same PlaybackEngine/SequenceStore
    // calls the local /api/record/start|save routes make (see WebApi.cpp) —
    // the servo moves and the buffer fills via the ordinary RECORDING-mode
    // path in PlaybackEngine::tick(), since onNetworkCommand already leaves
    // RECORDING mode untouched.
    networkLink.onSeqStart([]() {
      // A Master resends SEQ_START several times for delivery robustness —
      // a lost one is otherwise invisible, since the servo still moves on
      // ordinary CMD packets regardless of recording state (see
      // onNetworkCommand above). Without this guard, a duplicate that's
      // merely delayed rather than lost — arriving after capture is
      // already under way — would silently reset the in-progress
      // recording back to empty. Once actually recording, further starts
      // are a no-op until the matching SEQ_STOP.
      if (playback.mode() != PlaybackMode::RECORDING) {
        sequenceStore.startRecording();
        playback.startRecording(millis());
      }
    });
    networkLink.onSeqStop([](const char *name) {
      playback.stopRecording();
      sequenceStore.stopRecording();
      // SaveResult and SeqAckStatus share the same numeric reason codes by
      // design (SequenceStore stays free of any NetworkLink/protocol
      // dependency) — see both enums' definitions.
      SaveResult result = sequenceStore.saveAs(name);
      networkLink.sendSeqAck(name, static_cast<SeqAckStatus>(result), sequenceStore.recordedPointCount());
    });
    networkLink.onSeqClear([]() { sequenceStore.clearAll(); });
    networkLink.onSpaceQuery([]() { networkLink.sendSpaceReply(SequenceStore::freeSpaceBytes()); });
  }
  playback.begin(sink, &sequenceStore, relaySink);
  playback.setAngleLimits(settings.servoX.minAngle, settings.servoX.maxAngle, settings.servoY.minAngle,
                          settings.servoY.maxAngle);

  if (settings.networkMode != OperatingMode::MASTER) {
    // Applied before the web server comes up: a configured pattern/sequence
    // is already looping with zero user interaction, surviving reset/power-cycle.
    playback.applyAutostart(settings, millis());
    if (settings.autostartEnabled) {
      Serial.printf("[SELFTEST] autostart engaged: mode=%d\n", static_cast<int>(playback.mode()));
    } else {
      Serial.println("[SELFTEST] autostart disabled");
    }
  }

  webApi.begin(&playback, &sequenceStore, &settingsStore, &servos, &relay, &networkLink, sink, AP_IP);
  Serial.println("[SELFTEST] webserver started, ws clients=0");

  Serial.printf("[SELFTEST] free heap=%u bytes, network mode=%d node_id=%u\n", ESP.getFreeHeap(),
                static_cast<int>(settings.networkMode), settings.nodeId);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastTickMs >= TICK_INTERVAL_MS) {
    lastTickMs = now;
    playback.tick(now);
    networkLink.reportLocalAngles(servos.getX(), servos.getY());
    networkLink.reportLocalRelay(relay.relayState());

    if (bootNetworkMode == OperatingMode::NODE) {
      const bool motorActive = playback.mode() != PlaybackMode::IDLE;
      if (motorActive != nodeMotorActive) {
        nodeMotorActive = motorActive;
        WiFi.setTxPower(static_cast<wifi_power_t>(
            motorActive ? NODE_WIFI_TX_POWER_ACTIVE : WIFI_TX_POWER_MAX));
      }
    }
  }

  networkLink.loopTick(now);
  if (bootNetworkMode == OperatingMode::MASTER) {
    serialBridge.loopTick();
  }

  webApi.loopTick(now);
}

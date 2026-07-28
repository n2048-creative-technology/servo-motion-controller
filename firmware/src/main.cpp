#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include "Config.h"
#include "IAngleSink.h"
#include "ServoController.h"
#include "SettingsStore.h"
#include "SequenceStore.h"
#include "PlaybackEngine.h"
#include "WebApi.h"
#include "NetworkLink.h"
#include "NetworkAngleSink.h"
#include "SerialBridge.h"

ServoController servo;
SettingsStore settingsStore;
SequenceStore sequenceStore;
PlaybackEngine playback;
WebApi webApi;
NetworkLink networkLink;
NetworkAngleSink networkAngleSink;
SerialBridge serialBridge;

static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_NETMASK(255, 255, 255, 0);

uint32_t lastTickMs = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[SELFTEST] booting servo-motion-controller");

  settingsStore.load();
  const PersistedSettings &settings = settingsStore.settings();
  Serial.printf("[SELFTEST] settings loaded (version=%u, autostart=%d)\n", settings.version,
                settings.autostartEnabled ? 1 : 0);

  servo.begin(SERVO_PIN, settings.servoMinUs, settings.servoMaxUs, settings.servoMinAngle,
              settings.servoMaxAngle);
  servo.writeAngle(settings.servoCenterAngle);
  Serial.printf("[SELFTEST] servo attached pin=%d range=%u-%uus\n", SERVO_PIN, settings.servoMinUs,
                settings.servoMaxUs);

  bool fsOk = LittleFS.begin(true);
  Serial.printf("[SELFTEST] littlefs mount %s\n", fsOk ? "OK" : "FAILED");

  sequenceStore.begin();
  bool seqLoaded = sequenceStore.loadFromFS();
  if (seqLoaded) {
    Serial.printf("[SELFTEST] sequence file: present, %u points, %ums\n", sequenceStore.pointCount(),
                  sequenceStore.durationMs());
  } else {
    Serial.println("[SELFTEST] sequence file: none");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_NETMASK);
  const bool hasPassword = strlen(settings.apPassword) >= 8;
  const bool apOk = hasPassword ? WiFi.softAP(settings.apSsid, settings.apPassword, AP_WIFI_CHANNEL)
                                 : WiFi.softAP(settings.apSsid, nullptr, AP_WIFI_CHANNEL);
  Serial.printf("[SELFTEST] wifi AP %s ssid=%s ip=%s\n", apOk ? "up" : "FAILED", settings.apSsid,
                WiFi.softAPIP().toString().c_str());

  networkLink.begin(settings.networkMode, settings.nodeId);

  // MASTER drives selected Node(s) over ESP-NOW instead of a local servo, so
  // PlaybackEngine's jog/pattern/sequence logic is repointed at a
  // NetworkAngleSink; every other mode drives the physical ServoController
  // exactly as in v1. Autostart only makes sense with a real attached servo.
  IAngleSink *sink = &servo;
  if (settings.networkMode == OperatingMode::MASTER) {
    networkAngleSink.begin(&networkLink);
    sink = &networkAngleSink;
    serialBridge.begin(&networkLink);
  } else if (settings.networkMode == OperatingMode::NODE) {
    networkLink.onNodeCommand([](float angleDeg) { playback.onNetworkCommand(angleDeg, millis()); });
  }
  playback.begin(sink, &sequenceStore);

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

  webApi.begin(&playback, &sequenceStore, &settingsStore, &servo, &networkLink, sink, AP_IP);
  Serial.println("[SELFTEST] webserver started, ws clients=0");

  Serial.printf("[SELFTEST] free heap=%u bytes, network mode=%d node_id=%u\n", ESP.getFreeHeap(),
                static_cast<int>(settings.networkMode), settings.nodeId);
}

void loop() {
  const uint32_t now = millis();

  if (now - lastTickMs >= TICK_INTERVAL_MS) {
    lastTickMs = now;
    playback.tick(now);
    networkLink.reportLocalAngle(servo.getAngle());
  }

  networkLink.loopTick(now);
  if (settingsStore.settings().networkMode == OperatingMode::MASTER) {
    serialBridge.loopTick();
  }

  webApi.loopTick(now);
}

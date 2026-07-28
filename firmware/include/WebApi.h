#pragma once

#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include <IPAddress.h>
#include <stdint.h>

class PlaybackEngine;
class SequenceStore;
class SettingsStore;
class ServoController;
class NetworkLink;

// Owns the HTTP/WebSocket server and the captive-portal DNS catch-all.
// Registers all /api/* routes and serves the LittleFS-hosted single-page app.
class WebApi {
public:
  void begin(PlaybackEngine *playback, SequenceStore *sequence, SettingsStore *settingsStore,
             ServoController *servo, NetworkLink *network, const IPAddress &apIp);

  // Call every loop() iteration: services DNS captive-portal requests and
  // throttles the periodic WebSocket status broadcast internally.
  void loopTick(uint32_t now);

private:
  AsyncWebServer server_{80};
  AsyncWebSocket ws_{"/ws"};
  DNSServer dns_;

  PlaybackEngine *playback_ = nullptr;
  SequenceStore *sequence_ = nullptr;
  SettingsStore *settingsStore_ = nullptr;
  ServoController *servo_ = nullptr;
  NetworkLink *network_ = nullptr;

  uint32_t lastStatusBroadcastMs_ = 0;

  void setupRoutes();
  void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                 uint8_t *data, size_t len);
  void broadcastStatus();
  String buildStatusJson();
};

#include "SettingsStore.h"

#include <Preferences.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>

void SettingsStore::generateDefaultSsid(char *out, size_t outLen) {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(out, outLen, "%s%02X%02X%02X", AP_SSID_PREFIX, mac[3], mac[4], mac[5]);
}

void SettingsStore::factoryDefaults() {
  settings_ = PersistedSettings{};
  generateDefaultSsid(settings_.apSsid, sizeof(settings_.apSsid));
  strncpy(settings_.apPassword, AP_DEFAULT_PASSWORD, sizeof(settings_.apPassword) - 1);
  settings_.autostartPatternX = PatternParams{};
  settings_.autostartPatternY = PatternParams{};

  // Derive a default node id from the MAC so freshly-flashed boards don't all
  // collide on id 1 before the user assigns one in Settings.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  uint8_t range = NET_NODE_ID_MAX - NET_NODE_ID_MIN + 1;
  settings_.nodeId = NET_NODE_ID_MIN + (mac[5] % range);
}

void SettingsStore::load() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  PersistedSettings loaded;
  size_t got = prefs.getBytes(NVS_KEY, &loaded, sizeof(loaded));
  prefs.end();

  if (got == sizeof(loaded) && loaded.magic == SETTINGS_MAGIC && loaded.version == SETTINGS_VERSION) {
    settings_ = loaded;
    // Defensive null-termination in case of a corrupted blob.
    settings_.apSsid[sizeof(settings_.apSsid) - 1] = '\0';
    settings_.apPassword[sizeof(settings_.apPassword) - 1] = '\0';
  } else {
    factoryDefaults();
    save();
  }

  // Always runs last, whether the block above did a normal load or a
  // version-mismatch factory reset: puts the persisted SSID/password/
  // network mode/node id back regardless, since those live independently of
  // SETTINGS_VERSION. See Config.h's NET_IDENTITY_NVS_KEY comment.
  loadNetworkIdentity();
}

void SettingsStore::save() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putBytes(NVS_KEY, &settings_, sizeof(settings_));
  prefs.end();
  saveNetworkIdentity();
}

void SettingsStore::loadNetworkIdentity() {
  struct NetworkIdentity {
    uint32_t magic = 0;
    char apSsid[33] = {0};
    char apPassword[65] = {0};
    OperatingMode networkMode = OperatingMode::STANDALONE;
    uint8_t nodeId = NET_NODE_ID_MIN;
  };

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  NetworkIdentity id;
  size_t got = prefs.getBytes(NET_IDENTITY_NVS_KEY, &id, sizeof(id));
  prefs.end();

  if (got == sizeof(id) && id.magic == NET_IDENTITY_MAGIC) {
    strncpy(settings_.apSsid, id.apSsid, sizeof(settings_.apSsid) - 1);
    settings_.apSsid[sizeof(settings_.apSsid) - 1] = '\0';
    strncpy(settings_.apPassword, id.apPassword, sizeof(settings_.apPassword) - 1);
    settings_.apPassword[sizeof(settings_.apPassword) - 1] = '\0';
    settings_.networkMode = id.networkMode;
    settings_.nodeId = id.nodeId;
  } else {
    // No identity saved under this key yet (a board's first boot on
    // firmware that has this fix, or a genuinely first-ever boot) — seed it
    // from whatever settings_ already holds so it exists from here on.
    saveNetworkIdentity();
  }
}

void SettingsStore::saveNetworkIdentity() {
  struct NetworkIdentity {
    uint32_t magic = NET_IDENTITY_MAGIC;
    char apSsid[33] = {0};
    char apPassword[65] = {0};
    OperatingMode networkMode = OperatingMode::STANDALONE;
    uint8_t nodeId = NET_NODE_ID_MIN;
  };

  NetworkIdentity id;
  strncpy(id.apSsid, settings_.apSsid, sizeof(id.apSsid) - 1);
  strncpy(id.apPassword, settings_.apPassword, sizeof(id.apPassword) - 1);
  id.networkMode = settings_.networkMode;
  id.nodeId = settings_.nodeId;

  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putBytes(NET_IDENTITY_NVS_KEY, &id, sizeof(id));
  prefs.end();
}

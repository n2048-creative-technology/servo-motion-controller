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
}

void SettingsStore::save() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putBytes(NVS_KEY, &settings_, sizeof(settings_));
  prefs.end();
}

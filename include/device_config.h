#pragma once

#include <Arduino.h>

struct DeviceConfig {
  String factoryId;
  String factorySecret;
  String apiBaseUrl;
  String githubRepo;
  String firmwareAsset;
  String gameAssetBaseUrl;
  bool gameModeOnly = false;
  bool audioMuted = false;
};

// Load the device's factory configuration from NVS. A provisioning build
// seeds these values once; OTA builds only read the existing values.
bool initDeviceConfig();
const DeviceConfig& deviceConfig();
// Drop pairing, API, and update strings. Game files keep their own address.
void releaseDeviceConfigForGames();

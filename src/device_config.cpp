#include "device_config.h"

#include <Preferences.h>

#ifndef FALCON_CONFIG_PROVISIONING
#define FALCON_CONFIG_PROVISIONING 0
#endif

namespace {
constexpr char CONFIG_NAMESPACE[] = "falcon_cfg";
constexpr char CONFIG_READY_KEY[] = "config_ready";
constexpr char FACTORY_ID_KEY[] = "factory_id";
constexpr char FACTORY_SECRET_KEY[] = "factory_secret";
constexpr char API_BASE_URL_KEY[] = "api_base_url";
constexpr char GITHUB_REPO_KEY[] = "github_repo";
constexpr char FIRMWARE_ASSET_KEY[] = "firmware_asset";
constexpr char GAME_ASSET_URL_KEY[] = "game_asset_url";
constexpr char GAME_MODE_KEY[] = "game_mode";
constexpr char AUDIO_MUTED_KEY[] = "audio_muted";

DeviceConfig config;
bool configInitialized = false;

#if FALCON_CONFIG_PROVISIONING
#ifndef FALCON_PROVISION_FACTORY_ID
#define FALCON_PROVISION_FACTORY_ID ""
#endif
#ifndef FALCON_PROVISION_FACTORY_SECRET
#define FALCON_PROVISION_FACTORY_SECRET ""
#endif
#ifndef FALCON_PROVISION_API_BASE_URL
#define FALCON_PROVISION_API_BASE_URL ""
#endif
#ifndef FALCON_PROVISION_GITHUB_REPO
#define FALCON_PROVISION_GITHUB_REPO ""
#endif
#ifndef FALCON_PROVISION_FIRMWARE_ASSET
#define FALCON_PROVISION_FIRMWARE_ASSET "firmware.bin"
#endif
#ifndef FALCON_PROVISION_GAME_ASSET_URL
#define FALCON_PROVISION_GAME_ASSET_URL ""
#endif
#ifndef FALCON_PROVISION_GAME_MODE_ONLY
#define FALCON_PROVISION_GAME_MODE_ONLY 0
#endif
#ifndef FALCON_PROVISION_AUDIO_MUTED
#define FALCON_PROVISION_AUDIO_MUTED 0
#endif

void seedProvisionedConfig(Preferences& preferences) {
  preferences.putString(FACTORY_ID_KEY, FALCON_PROVISION_FACTORY_ID);
  preferences.putString(FACTORY_SECRET_KEY, FALCON_PROVISION_FACTORY_SECRET);
  preferences.putString(API_BASE_URL_KEY, FALCON_PROVISION_API_BASE_URL);
  preferences.putString(GITHUB_REPO_KEY, FALCON_PROVISION_GITHUB_REPO);
  preferences.putString(FIRMWARE_ASSET_KEY, FALCON_PROVISION_FIRMWARE_ASSET);
  preferences.putString(GAME_ASSET_URL_KEY, FALCON_PROVISION_GAME_ASSET_URL);
  preferences.putBool(GAME_MODE_KEY, FALCON_PROVISION_GAME_MODE_ONLY != 0);
  preferences.putBool(AUDIO_MUTED_KEY, FALCON_PROVISION_AUDIO_MUTED != 0);
  preferences.putBool(CONFIG_READY_KEY, true);
}
#endif

}  // namespace

bool initDeviceConfig() {
  if (configInitialized) {
    return true;
  }

  Preferences preferences;
  if (!preferences.begin(CONFIG_NAMESPACE, false)) {
    return false;
  }

#if FALCON_CONFIG_PROVISIONING
  // Explicit provisioning is used only for the first physical flash (or an
  // intentional factory re-provision). Normal OTA builds never enter this
  // branch and therefore cannot overwrite the device's settings.
  seedProvisionedConfig(preferences);
#endif

  const bool ready = preferences.getBool(CONFIG_READY_KEY, false);
  if (ready) {
    config.factoryId = preferences.getString(FACTORY_ID_KEY, "");
    config.factorySecret = preferences.getString(FACTORY_SECRET_KEY, "");
    config.apiBaseUrl = preferences.getString(API_BASE_URL_KEY, "");
    config.githubRepo = preferences.getString(GITHUB_REPO_KEY, "");
    config.firmwareAsset = preferences.getString(FIRMWARE_ASSET_KEY,
                                                  "firmware.bin");
    config.gameAssetBaseUrl = preferences.getString(GAME_ASSET_URL_KEY, "");
    config.gameModeOnly = preferences.getBool(GAME_MODE_KEY, false);
    config.audioMuted = preferences.getBool(AUDIO_MUTED_KEY, false);
  }

  preferences.end();
  configInitialized = ready;
  return configInitialized;
}

const DeviceConfig& deviceConfig() {
  return config;
}

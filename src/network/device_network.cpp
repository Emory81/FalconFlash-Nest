#include "network/device_network.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>

#include "device_config.h"

namespace {
constexpr char WIFI_SSID[] = "Messiah Guest";
constexpr char WIFI_PASSWORD[] = "";
constexpr char WIFI_CHANNEL_KEY[] = "wifi_channel";
constexpr char WIFI_BSSID_KEY[] = "wifi_bssid";
constexpr char OTA_RESUME_BUTTON_KEY[] = "ota_resume_btn";
#ifndef FALCON_FIRMWARE_VERSION
#define FALCON_FIRMWARE_VERSION "0.0.0-dev"
#endif
constexpr char FIRMWARE_VERSION[] = FALCON_FIRMWARE_VERSION;
constexpr char PAIRING_CODE_PATH[] = "/nest/device/pairing-code";
constexpr char PAIRING_STATUS_PATH[] = "/nest/device/pairing-status";
constexpr char PRICE_PATH[] = "/nest/device/price";
constexpr char ORDER_PATH[] = "/nest/device/order";
constexpr char ORDER_STATUS_PATH[] = "/order_status";
constexpr uint8_t PICKUP_LOCATION_COUNT = 5;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t WIFI_HINT_FALLBACK_MS = 2000;
constexpr uint32_t FIRMWARE_WIFI_WAIT_MS = 10000;
constexpr uint32_t OTA_STREAM_TIMEOUT_MS = 15000;
constexpr uint32_t PAIRING_POLL_MS = 2000;
constexpr uint32_t DEFAULT_PAIRING_EXPIRY_MS = 600000;
bool wifiStarted = false;
bool wifiHintActive = false;
uint32_t wifiAttemptStartedAt = 0;

void drawCenteredText(TFT_eSPI& display, const char* text, uint8_t size,
                      int16_t y) {
  display.setTextFont(1);
  display.setTextSize(size);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.drawString(text, display.width() / 2, y);
}

void drawPairingScreen(TFT_eSPI& display, const String& code,
                       const char* status) {
  display.fillScreen(TFT_BLACK);
  drawCenteredText(display, "Pair Device", 2, 22);
  drawCenteredText(display, "Enter this code:", 1, 56);
  drawCenteredText(display, code.c_str(), 4, 106);
  drawCenteredText(display, "Go to: falconflash.co/nest", 1, 146);
  drawCenteredText(display, status, 1, 174);
}

String extractJsonString(const String& json, const char* key) {
  const String keyToken = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(keyToken);
  if (keyStart < 0) {
    return String();
  }

  const int colon = json.indexOf(':', keyStart + keyToken.length());
  if (colon < 0) {
    return String();
  }
  const int openingQuote = json.indexOf('"', colon + 1);
  if (openingQuote < 0) {
    return String();
  }
  const int closingQuote = json.indexOf('"', openingQuote + 1);
  if (closingQuote < 0) {
    return String();
  }
  return json.substring(openingQuote + 1, closingQuote);
}

String extractJsonNumber(const String& json, const char* key) {
  const String keyToken = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(keyToken);
  if (keyStart < 0) {
    return String();
  }

  const int colon = json.indexOf(':', keyStart + keyToken.length());
  if (colon < 0) {
    return String();
  }

  int valueStart = colon + 1;
  while (valueStart < json.length() &&
         (json[valueStart] == ' ' || json[valueStart] == '\n' ||
          json[valueStart] == '\r' || json[valueStart] == '\t')) {
    ++valueStart;
  }

  int valueEnd = valueStart;
  while (valueEnd < json.length() &&
         (isDigit(json[valueEnd]) || json[valueEnd] == '.' ||
          (valueEnd == valueStart && json[valueEnd] == '-'))) {
    ++valueEnd;
  }
  return json.substring(valueStart, valueEnd);
}

uint8_t extractJsonIdArrayMask(const String& json, const char* key) {
  const String keyToken = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(keyToken);
  if (keyStart < 0) {
    return 0;
  }

  const int colon = json.indexOf(':', keyStart + keyToken.length());
  const int arrayStart = colon < 0 ? -1 : json.indexOf('[', colon + 1);
  if (arrayStart < 0) {
    return 0;
  }
  const int arrayEnd = json.indexOf(']', arrayStart + 1);
  if (arrayEnd < 0) {
    return 0;
  }

  uint8_t mask = 0;
  int cursor = arrayStart + 1;
  while (cursor < arrayEnd) {
    while (cursor < arrayEnd && !isDigit(json[cursor])) {
      ++cursor;
    }
    if (cursor >= arrayEnd) {
      break;
    }

    int valueEnd = cursor;
    while (valueEnd < arrayEnd && isDigit(json[valueEnd])) {
      ++valueEnd;
    }
    const int locationId = json.substring(cursor, valueEnd).toInt();
    if (locationId >= 1 && locationId <= PICKUP_LOCATION_COUNT) {
      mask |= static_cast<uint8_t>(1U << (locationId - 1));
    }
    cursor = valueEnd;
  }
  return mask;
}

String extractJsonScalar(const String& json, const char* key) {
  const String keyToken = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(keyToken);
  if (keyStart < 0) {
    return String();
  }

  const int colon = json.indexOf(':', keyStart + keyToken.length());
  if (colon < 0) {
    return String();
  }

  int valueStart = colon + 1;
  while (valueStart < json.length() &&
         (json[valueStart] == ' ' || json[valueStart] == '\n' ||
          json[valueStart] == '\r' || json[valueStart] == '\t')) {
    ++valueStart;
  }
  if (valueStart >= json.length()) {
    return String();
  }

  if (json[valueStart] == '"') {
    const int closingQuote = json.indexOf('"', valueStart + 1);
    if (closingQuote < 0) {
      return String();
    }
    return json.substring(valueStart + 1, closingQuote);
  }

  int valueEnd = valueStart;
  while (valueEnd < json.length() && json[valueEnd] != ',' &&
         json[valueEnd] != '}') {
    ++valueEnd;
  }

  String value = json.substring(valueStart, valueEnd);
  value.trim();
  if (value == "null") {
    return String();
  }
  return value;
}

bool jsonValueIsTrue(const String& json, const char* key) {
  const String keyToken = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(keyToken);
  if (keyStart < 0) {
    return false;
  }
  const int colon = json.indexOf(':', keyStart + keyToken.length());
  if (colon < 0) {
    return false;
  }
  return json.indexOf("true", colon + 1) >= 0;
}

bool jsonFlagIsSet(const String& json, const char* key) {
  String value = extractJsonScalar(json, key);
  value.toLowerCase();
  return value.length() > 0 && value != "false" && value != "0";
}

bool responseIndicatesUnarmed(const String& response) {
  String normalizedResponse = response;
  normalizedResponse.toLowerCase();
  if (normalizedResponse.indexOf("not armed") >= 0 ||
      normalizedResponse.indexOf("not_armed") >= 0 ||
      normalizedResponse.indexOf("button_not_armed") >= 0 ||
      normalizedResponse.indexOf("unarmed") >= 0) {
    return true;
  }

  const String armedValue = extractJsonScalar(normalizedResponse, "armed");
  const String isArmedValue =
      extractJsonScalar(normalizedResponse, "is_armed");
  if (armedValue == "false" || armedValue == "0" ||
      isArmedValue == "false" || isArmedValue == "0") {
    return true;
  }
  return false;
}

bool loadWiFiHint(uint8_t& channel, uint8_t* bssid) {
  Preferences preferences;
  preferences.begin("falcon", true);
  channel = preferences.getUChar(WIFI_CHANNEL_KEY, 0);
  const size_t bytesRead =
      preferences.getBytes(WIFI_BSSID_KEY, bssid, 6);
  preferences.end();
  return channel > 0 && bytesRead == 6;
}

void saveWiFiHint() {
  if (WiFi.status() != WL_CONNECTED || WiFi.channel() <= 0) {
    return;
  }

  const uint8_t* bssid = WiFi.BSSID();
  if (bssid == nullptr) {
    return;
  }

  Preferences preferences;
  preferences.begin("falcon", false);
  preferences.putUChar(WIFI_CHANNEL_KEY,
                       static_cast<uint8_t>(WiFi.channel()));
  preferences.putBytes(WIFI_BSSID_KEY, bssid, 6);
  preferences.end();
}

bool connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    saveWiFiHint();
    return true;
  }

  if (!wifiStarted) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiStarted = true;
    wifiAttemptStartedAt = millis();
  }

  uint32_t attemptStartedAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - attemptStartedAt >= WIFI_RETRY_MS) {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      attemptStartedAt = millis();
    }
    delay(250);
  }
  saveWiFiHint();
  return true;
}

int postJson(const char* path, const String& body, bool useFactoryHeaders,
             const String& bearerToken, String& response) {
  WiFiClientSecure client;
  client.setInsecure();

  const DeviceConfig& config = deviceConfig();
  HTTPClient http;
  const String url = config.apiBaseUrl + path;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(10000);
  http.addHeader("X-Firmware-Version", FIRMWARE_VERSION);

  if (useFactoryHeaders) {
    http.addHeader("X-Nest-Factory-Id", config.factoryId);
    http.addHeader("X-Nest-Factory-Secret", config.factorySecret);
  }
  if (bearerToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + bearerToken);
  }
  if (body.length() > 0) {
    http.addHeader("Content-Type", "application/json");
  }

  const int status = http.POST(body);
  response = http.getString();
  http.end();
  return status;
}

int getJson(const String& path, const String& bearerToken, String& response) {
  WiFiClientSecure client;
  client.setInsecure();

  const DeviceConfig& config = deviceConfig();
  HTTPClient http;
  const String url = config.apiBaseUrl + path;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(10000);
  http.addHeader("X-Firmware-Version", FIRMWARE_VERSION);
  if (bearerToken.length() > 0) {
    http.addHeader("Authorization", "Bearer " + bearerToken);
  }

  const int status = http.GET();
  response = http.getString();
  http.end();
  return status;
}

int getJson(const String& path, String& response) {
  return getJson(path, String(), response);
}

int firmwareVersionPart(const String& version, uint8_t partIndex) {
  String normalized = version;
  normalized.trim();
  if (normalized.startsWith("v") || normalized.startsWith("V")) {
    normalized.remove(0, 1);
  }

  int partStart = 0;
  for (uint8_t index = 0; index < partIndex; ++index) {
    const int separator = normalized.indexOf('.', partStart);
    if (separator < 0) {
      return 0;
    }
    partStart = separator + 1;
  }

  int partEnd = normalized.indexOf('.', partStart);
  if (partEnd < 0) {
    partEnd = normalized.length();
  }
  if (partEnd <= partStart) {
    return 0;
  }
  return normalized.substring(partStart, partEnd).toInt();
}

bool isNewerFirmwareVersion(const String& candidateVersion) {
  if (candidateVersion.length() == 0) {
    return false;
  }

  for (uint8_t index = 0; index < 3; ++index) {
    const int candidatePart = firmwareVersionPart(candidateVersion, index);
    const int currentPart = firmwareVersionPart(FIRMWARE_VERSION, index);
    if (candidatePart != currentPart) {
      return candidatePart > currentPart;
    }
  }
  return false;
}

void drawOtaProgress(TFT_eSPI& display, uint8_t percent) {
  const int16_t textY = display.height() - 14;
  display.fillRect(0, textY - 2, 118, 16, TFT_BLACK);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  char text[24];
  snprintf(text, sizeof(text), "Updating: %u%%", percent);
  display.drawString(text, 4, textY);
}

bool installFirmwareUpdate(const String& firmwareUrl, TFT_eSPI& display,
                           uint8_t wakeButtonNumber) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, firmwareUrl)) {
    return false;
  }
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  const size_t updateSize =
      contentLength > 0 ? static_cast<size_t>(contentLength)
                        : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize)) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  size_t written = 0;
  uint32_t lastDataAt = millis();
  uint8_t lastPercent = 255;
  bool writeOk = stream != nullptr;
  drawOtaProgress(display, 0);
  while (writeOk &&
         (contentLength <= 0 || written < static_cast<size_t>(contentLength)) &&
         (http.connected() || stream->available())) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataAt >= OTA_STREAM_TIMEOUT_MS) {
        writeOk = false;
        break;
      }
      delay(2);
      continue;
    }

    size_t requested = min<size_t>(sizeof(buffer), available);
    if (contentLength > 0) {
      requested =
          min<size_t>(requested, static_cast<size_t>(contentLength) - written);
    }
    const size_t received = stream->readBytes(buffer, requested);
    if (received == 0 || Update.write(buffer, received) != received) {
      writeOk = false;
      break;
    }
    written += received;
    lastDataAt = millis();
    if (contentLength > 0) {
      const uint8_t percent = static_cast<uint8_t>(
          min<size_t>(100, written * 100 / static_cast<size_t>(contentLength)));
      if (percent != lastPercent) {
        drawOtaProgress(display, percent);
        lastPercent = percent;
      }
    }
  }
  const bool sizeMatches =
      contentLength <= 0 || written == static_cast<size_t>(contentLength);
  if (!writeOk || !sizeMatches || !Update.end()) {
    Update.abort();
    http.end();
    return false;
  }

  drawOtaProgress(display, 100);
  http.end();
  Preferences preferences;
  preferences.begin("falcon", false);
  preferences.putUChar(OTA_RESUME_BUTTON_KEY, wakeButtonNumber);
  preferences.end();
  delay(500);
  esp_restart();
  return true;
}

String githubRepositoryPath() {
  String repository = deviceConfig().githubRepo;
  repository.trim();

  const int githubHostStart = repository.indexOf("github.com/");
  if (githubHostStart >= 0) {
    repository = repository.substring(githubHostStart + 11);
  }

  while (repository.startsWith("/")) {
    repository.remove(0, 1);
  }
  const int queryStart = repository.indexOf('?');
  if (queryStart >= 0) {
    repository.remove(queryStart);
  }
  const int fragmentStart = repository.indexOf('#');
  if (fragmentStart >= 0) {
    repository.remove(fragmentStart);
  }
  while (repository.endsWith("/")) {
    repository.remove(repository.length() - 1);
  }
  if (repository.endsWith(".git")) {
    repository.remove(repository.length() - 4);
  }
  return repository;
}

String findFirmwareAssetUrl(const String& releaseJson) {
  const int assetsKeyStart = releaseJson.indexOf("\"assets\"");
  if (assetsKeyStart < 0) {
    return String();
  }

  int searchFrom = releaseJson.indexOf('[', assetsKeyStart);
  while (searchFrom >= 0) {
    const int nameKeyStart = releaseJson.indexOf("\"name\"", searchFrom);
    if (nameKeyStart < 0) {
      return String();
    }
    const String assetJson = releaseJson.substring(nameKeyStart);
    const String assetName = extractJsonString(assetJson, "name");
    if (assetName == deviceConfig().firmwareAsset) {
      return extractJsonString(assetJson, "browser_download_url");
    }
    searchFrom = nameKeyStart + 6;
  }
  return String();
}

bool checkForFirmwareUpdateInternal(TFT_eSPI& display,
                                    uint8_t wakeButtonNumber) {
  if (deviceConfig().githubRepo.length() == 0 ||
      WiFi.status() != WL_CONNECTED) {
    return false;
  }

  const String repository = githubRepositoryPath();
  if (repository.length() == 0) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const String latestReleaseUrl =
      "https://api.github.com/repos/" + repository + "/releases/latest";
  if (!http.begin(client, latestReleaseUrl)) {
    return false;
  }
  http.setTimeout(10000);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("User-Agent",
                 String("FalconFlash-Nest/") + FIRMWARE_VERSION);

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const String releaseJson = http.getString();
  http.end();

  const String candidateVersion = extractJsonString(releaseJson, "tag_name");
  const String firmwareUrl = findFirmwareAssetUrl(releaseJson);
  if (!isNewerFirmwareVersion(candidateVersion) || firmwareUrl.length() == 0) {
    return false;
  }

  return installFirmwareUpdate(firmwareUrl, display, wakeButtonNumber);
}

String loadDeviceToken() {
  Preferences preferences;
  preferences.begin("falcon", true);
  const String token = preferences.getString("device_token", "");
  preferences.end();
  return token;
}

void clearDeviceToken() {
  Preferences preferences;
  preferences.begin("falcon", false);
  preferences.remove("device_token");
  preferences.end();
}

void saveDeviceToken(const String& token) {
  Preferences preferences;
  preferences.begin("falcon", false);
  preferences.putString("device_token", token);
  preferences.end();
}

int findJsonObjectEnd(const String& json, int objectStart) {
  bool inString = false;
  bool escaped = false;
  int depth = 0;
  for (int index = objectStart; index < json.length(); ++index) {
    const char character = json[index];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        inString = false;
      }
      continue;
    }
    if (character == '"') {
      inString = true;
    } else if (character == '{') {
      ++depth;
    } else if (character == '}' && --depth == 0) {
      return index;
    }
  }
  return -1;
}

bool parseDeviceMessage(const String& object, DeviceMessage& message) {
  const String messageId = extractJsonNumber(object, "message_id");
  if (messageId.length() == 0) {
    return false;
  }

  message.messageId = static_cast<uint32_t>(messageId.toInt());
  message.buttonNumber =
      static_cast<uint8_t>(extractJsonNumber(object, "button_number").toInt());
  message.flyerName = extractJsonString(object, "flyer_name");
  message.message = extractJsonString(object, "message");
  message.createdAt = extractJsonString(object, "created_at");
  return true;
}

uint8_t parseDeviceMessages(const String& response, DeviceMessage* messages,
                            uint8_t maxMessages) {
  if (messages == nullptr || maxMessages == 0) {
    return 0;
  }

  const int messagesKey = response.indexOf("\"messages\"");
  if (messagesKey < 0) {
    return 0;
  }
  const int arrayStart = response.indexOf('[', messagesKey);
  if (arrayStart < 0) {
    return 0;
  }

  uint8_t count = 0;
  int cursor = arrayStart + 1;
  while (cursor < response.length() && count < maxMessages) {
    const int objectStart = response.indexOf('{', cursor);
    if (objectStart < 0) {
      break;
    }
    const int objectEnd = findJsonObjectEnd(response, objectStart);
    if (objectEnd < 0) {
      break;
    }
    DeviceMessage parsed;
    if (parseDeviceMessage(
            response.substring(objectStart, objectEnd + 1), parsed)) {
      messages[count++] = parsed;
    }
    cursor = objectEnd + 1;
  }
  return count;
}

String nextEventId() {
  Preferences preferences;
  preferences.begin("falcon", false);
  const uint32_t sequence = preferences.getUInt("event_sequence", 0) + 1;
  preferences.putUInt("event_sequence", sequence);
  preferences.end();

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return mac + "-" + String(sequence) + "-" +
         String(static_cast<uint32_t>(esp_random()), HEX);
}
}  // namespace

void startDeviceWiFi() {
  if (wifiStarted) {
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  uint8_t cachedChannel = 0;
  uint8_t cachedBssid[6] = {};
  wifiHintActive = loadWiFiHint(cachedChannel, cachedBssid);
  wifiAttemptStartedAt = millis();
  if (wifiHintActive) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, cachedChannel, cachedBssid, true);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  wifiStarted = true;
}

bool deviceWiFiConnected() {
  if (WiFi.status() != WL_CONNECTED && wifiHintActive &&
      millis() - wifiAttemptStartedAt >= WIFI_HINT_FALLBACK_MS) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiHintActive = false;
    wifiAttemptStartedAt = millis();
  }
  return WiFi.status() == WL_CONNECTED;
}

bool checkForFirmwareUpdate(TFT_eSPI& display, uint8_t wakeButtonNumber) {
  if (deviceConfig().githubRepo.length() == 0) {
    return false;
  }

  const uint32_t startedAt = millis();
  while (!deviceWiFiConnected() &&
         millis() - startedAt < FIRMWARE_WIFI_WAIT_MS) {
    delay(100);
  }
  return checkForFirmwareUpdateInternal(display, wakeButtonNumber);
}

uint8_t takeOtaResumeButton() {
  Preferences preferences;
  preferences.begin("falcon", false);
  const uint8_t button = preferences.getUChar(OTA_RESUME_BUTTON_KEY, 0);
  preferences.remove(OTA_RESUME_BUTTON_KEY);
  preferences.end();
  if (button < 1 || button > 4) {
    return 0;
  }
  return button;
}

bool ensureDevicePaired(TFT_eSPI& display) {
  String deviceToken = loadDeviceToken();
  connectToWiFi();
  if (deviceToken.length() > 0) {
    return true;
  }

  while (true) {
    String pairingResponse;
    const int pairingStatus =
        postJson(PAIRING_CODE_PATH, "", true, "", pairingResponse);
    const String code = extractJsonString(pairingResponse, "code");
    const String session =
        extractJsonString(pairingResponse, "pairing_session");
    if (pairingStatus < 200 || pairingStatus >= 300 || code.length() == 0 ||
        session.length() == 0) {
      drawPairingScreen(display, "", "Retrying pairing service...");
      delay(PAIRING_POLL_MS);
      continue;
    }

    uint32_t expiryMs = DEFAULT_PAIRING_EXPIRY_MS;
    const String expirySeconds =
        extractJsonNumber(pairingResponse, "expires_in_seconds");
    if (expirySeconds.length() > 0) {
      expiryMs = expirySeconds.toInt() * 1000UL;
    }
    drawPairingScreen(display, code, "Waiting for registration...");

    const uint32_t pairingStartedAt = millis();
    while (millis() - pairingStartedAt < expiryMs) {
      String statusResponse;
      const String statusBody =
          "{\"pairing_session\":\"" + session + "\"}";
      const int status = postJson(PAIRING_STATUS_PATH, statusBody, true, "",
                                  statusResponse);
      if (status >= 200 && status < 300 &&
          jsonValueIsTrue(statusResponse, "paired")) {
        deviceToken = extractJsonString(statusResponse, "device_token");
        if (deviceToken.length() > 0) {
          saveDeviceToken(deviceToken);
          drawPairingScreen(display, code, "Device registered");
          delay(1000);
          return true;
        }
      }
      delay(PAIRING_POLL_MS);
    }
  }
}

DevicePriceStatus fetchDevicePrice(uint8_t buttonNumber, String& priceText,
                                   uint8_t& savedPickupLocationMask) {
  savedPickupLocationMask = 0;
  if (buttonNumber < 1 || buttonNumber > 4) {
    return DevicePriceStatus::Unavailable;
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_RETRY_MS) {
      delay(250);
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    return DevicePriceStatus::Unavailable;
  }

  const String token = loadDeviceToken();
  if (token.length() == 0) {
    return DevicePriceStatus::NeedsPairing;
  }

  const String body = "{\"button_number\":" + String(buttonNumber) + "}";
  String response;
  const int status = postJson(PRICE_PATH, body, false, token, response);
  if (responseIndicatesUnarmed(response)) {
    return DevicePriceStatus::Unarmed;
  }
  if (status == 400 || status == 409 || status == 422) {
    // The price endpoint uses client-error responses for unavailable buttons.
    return DevicePriceStatus::Unarmed;
  }
  if (status == 401 || status == 403 || status == 404) {
    clearDeviceToken();
    return DevicePriceStatus::NeedsPairing;
  }
  if (status < 200 || status >= 300) {
    return DevicePriceStatus::Unavailable;
  }

  const String amount = extractJsonNumber(response, "amount");
  if (amount.length() == 0) {
    if (responseIndicatesUnarmed(response)) {
      return DevicePriceStatus::Unarmed;
    }
    return DevicePriceStatus::Unavailable;
  }

  savedPickupLocationMask =
      extractJsonIdArrayMask(response, "saved_pickup_location_ids");
  if (savedPickupLocationMask == 0) {
    savedPickupLocationMask =
        extractJsonIdArrayMask(response, "pickup_location_ids");
  }

  char formattedPrice[16];
  snprintf(formattedPrice, sizeof(formattedPrice), "$%.2f",
           amount.toFloat());
  priceText = formattedPrice;
  return DevicePriceStatus::Success;
}

DeviceOrderStatus submitDeviceOrder(uint8_t buttonNumber,
                                    uint8_t pickupLocationMask,
                                    String& eventId, String& orderId,
                                    String& failureDetail) {
  orderId = String();
  failureDetail = String();
  if (buttonNumber < 1 || buttonNumber > 4) {
    failureDetail = "Invalid button";
    return DeviceOrderStatus::Unavailable;
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < WIFI_RETRY_MS) {
      delay(250);
    }
  }
  if (WiFi.status() != WL_CONNECTED) {
    failureDetail = "Wi-Fi unavailable";
    return DeviceOrderStatus::Unavailable;
  }

  const String token = loadDeviceToken();
  if (token.length() == 0) {
    failureDetail = "Device needs pairing";
    return DeviceOrderStatus::NeedsPairing;
  }

  if (eventId.length() == 0) {
    eventId = nextEventId();
  }

  String body = "{\"button_number\":" + String(buttonNumber);
  if (pickupLocationMask != 0) {
    body += ",\"pickup_location_ids\":[";
    bool firstLocation = true;
    for (uint8_t location = 1; location <= PICKUP_LOCATION_COUNT;
         ++location) {
      if ((pickupLocationMask & (1U << (location - 1))) == 0) {
        continue;
      }
      if (!firstLocation) {
        body += ",";
      }
      body += String(location);
      firstLocation = false;
    }
    body += "]";
  }
  body += ",\"event_id\":\"" + eventId + "\"}";
  String response;
  const int status = postJson(ORDER_PATH, body, false, token, response);
  String responseMessage = extractJsonString(response, "error");
  if (responseMessage.length() == 0) {
    responseMessage = extractJsonString(response, "message");
  }
  failureDetail = String("HTTP ") + status;
  if (responseMessage.length() > 0) {
    failureDetail += ": " + responseMessage;
  }
  if (responseIndicatesUnarmed(response) || status == 400 || status == 409 ||
      status == 422) {
    return DeviceOrderStatus::Unarmed;
  }
  if (status == 401 || status == 403 || status == 404) {
    clearDeviceToken();
    return DeviceOrderStatus::NeedsPairing;
  }
  if (status < 200 || status >= 300) {
    return DeviceOrderStatus::Unavailable;
  }

  // The status endpoint accepts the public order number. Prefer it when the
  // order response includes both the internal ID and public order number.
  orderId = extractJsonScalar(response, "order_number");
  if (orderId.length() == 0) {
    orderId = extractJsonScalar(response, "order_id");
  }
  if (orderId.length() == 0) {
    orderId = extractJsonScalar(response, "id");
  }
  if (orderId.length() == 0 && status >= 200 && status < 300) {
    failureDetail = "Success response missing order ID";
  }
  return orderId.length() > 0 ? DeviceOrderStatus::Success
                              : DeviceOrderStatus::InvalidResponse;
}

bool fetchOrderStatus(const String& orderId, OrderStatus& status) {
  if (orderId.length() == 0) {
    return false;
  }
  if (!connectToWiFi()) {
    return false;
  }

  String response;
  const String path = String(ORDER_STATUS_PATH) + "?id=" + orderId;
  const int httpStatus = getJson(path, response);
  if (httpStatus < 200 || httpStatus >= 300) {
    return false;
  }

  status.orderStatus = extractJsonScalar(response, "order_status");
  status.orderNumber = extractJsonScalar(response, "order_number");
  status.orderPlacedAt = extractJsonScalar(response, "order_placed");
  status.orderPlaced = status.orderPlacedAt.length() > 0;
  status.customerOrderPlacedAt =
      extractJsonScalar(response, "customer_order_placed_at");
  const String flasherAssigned =
      extractJsonScalar(response, "flasher_assigned");
  status.flasherAssigned =
      flasherAssigned.length() > 0 && flasherAssigned != "false";
  status.flasherName = extractJsonScalar(response, "flasher_name");
  if (status.flasherName.length() == 0) {
    status.flasherName = extractJsonScalar(response, "flyer_name");
  }
  if (status.flasherName.length() == 0) {
    status.flasherName = extractJsonScalar(response, "flasher");
  }
  if (status.flasherName.length() == 0 &&
      flasherAssigned != "true" && flasherAssigned != "false" &&
      flasherAssigned != "0" && flasherAssigned != "1") {
    status.flasherName = flasherAssigned;
  }
  status.pickedUpAt = extractJsonScalar(response, "picked_up_at");
  if (status.pickedUpAt.length() == 0) {
    status.pickedUpAt =
        extractJsonScalar(response, "order_picked_up_at");
  }
  if (status.pickedUpAt.length() == 0) {
    status.pickedUpAt = extractJsonScalar(response, "picked_up");
  }
  status.deliveredAt = extractJsonScalar(response, "delivered_at");
  String normalizedOrderStatus = status.orderStatus;
  normalizedOrderStatus.toLowerCase();
  status.canceledOrRefunded =
      normalizedOrderStatus == "-1" ||
      normalizedOrderStatus.indexOf("cancel") >= 0 ||
      normalizedOrderStatus.indexOf("refund") >= 0 ||
      jsonFlagIsSet(response, "canceled") ||
      jsonFlagIsSet(response, "cancelled") ||
      jsonFlagIsSet(response, "refunded") ||
      jsonFlagIsSet(response, "order_canceled") ||
      jsonFlagIsSet(response, "order_cancelled") ||
      jsonFlagIsSet(response, "order_refunded") ||
      jsonFlagIsSet(response, "canceled_at") ||
      jsonFlagIsSet(response, "cancelled_at") ||
      jsonFlagIsSet(response, "refunded_at") ||
      jsonFlagIsSet(response, "order_canceled_at") ||
      jsonFlagIsSet(response, "order_cancelled_at") ||
      jsonFlagIsSet(response, "order_refunded_at");
  status.orderPhoto = extractJsonScalar(response, "order_photo");

  return status.orderStatus.length() > 0 ||
         status.orderNumber.length() > 0 ||
         status.orderPlacedAt.length() > 0 ||
         status.customerOrderPlacedAt.length() > 0 ||
         status.pickedUpAt.length() > 0 ||
         status.deliveredAt.length() > 0 ||
         status.canceledOrRefunded;
}

bool fetchAuthenticatedJson(const String& path, String& response,
                            int* httpStatusOut) {
  if (httpStatusOut != nullptr) {
    *httpStatusOut = 0;
  }
  const String token = loadDeviceToken();
  if (token.length() == 0) {
    return false;
  }
  if (!connectToWiFi()) {
    return false;
  }
  const int httpStatus = getJson(path, token, response);
  if (httpStatusOut != nullptr) {
    *httpStatusOut = httpStatus;
  }
  if (httpStatus == 401 || httpStatus == 403) {
    clearDeviceToken();
    return false;
  }
  return httpStatus >= 200 && httpStatus < 300;
}

DeviceMessagePollStatus fetchDeviceMessages(
    uint8_t buttonNumber, uint32_t sinceMessageId, DeviceMessage* messages,
    uint8_t maxMessages, uint8_t& messageCount,
    uint32_t& nextSinceMessageId) {
  messageCount = 0;
  nextSinceMessageId = sinceMessageId;

  const String token = loadDeviceToken();
  if (token.length() == 0) {
    return DeviceMessagePollStatus::NeedsPairing;
  }

  String path = "/nest/device/messages?limit=" + String(maxMessages);
  if (buttonNumber >= 1 && buttonNumber <= 4) {
    path += "&button_number=" + String(buttonNumber);
  }
  if (sinceMessageId > 0) {
    path += "&since_message_id=" + String(sinceMessageId);
  }

  String response;
  const int httpStatus = getJson(path, token, response);
  if (httpStatus == 401 || httpStatus == 403) {
    clearDeviceToken();
    return DeviceMessagePollStatus::NeedsPairing;
  }
  if (httpStatus < 200 || httpStatus >= 300) {
    return DeviceMessagePollStatus::Unavailable;
  }

  messageCount = parseDeviceMessages(response, messages, maxMessages);
  const String nextSince =
      extractJsonNumber(response, "next_since_message_id");
  if (nextSince.length() > 0) {
    nextSinceMessageId = static_cast<uint32_t>(nextSince.toInt());
  } else {
    for (uint8_t index = 0; index < messageCount; ++index) {
      if (messages[index].messageId > nextSinceMessageId) {
        nextSinceMessageId = messages[index].messageId;
      }
    }
  }
  return DeviceMessagePollStatus::Success;
}

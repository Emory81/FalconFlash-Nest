#include "device_network.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>

namespace {
constexpr char WIFI_SSID[] = "Messiah Guest";
constexpr char WIFI_PASSWORD[] = "";
constexpr char BASE_URL[] = "https://falconflash.emory.work/api";
#ifndef FALCON_FACTORY_ID
#error "FALCON_FACTORY_ID must be set in .env"
#endif
#ifndef FALCON_FACTORY_SECRET
#error "FALCON_FACTORY_SECRET must be set in .env"
#endif
constexpr char FACTORY_ID[] = FALCON_FACTORY_ID;
constexpr char FACTORY_SECRET[] = FALCON_FACTORY_SECRET;
constexpr char PAIRING_CODE_PATH[] = "/nest/device/pairing-code";
constexpr char PAIRING_STATUS_PATH[] = "/nest/device/pairing-status";
constexpr char PRICE_PATH[] = "/nest/device/price";
constexpr char ORDER_PATH[] = "/nest/device/order";
constexpr char ORDER_STATUS_PATH[] = "/order_status";
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t PAIRING_POLL_MS = 2000;
constexpr uint32_t DEFAULT_PAIRING_EXPIRY_MS = 600000;
bool wifiStarted = false;

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

bool connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  if (!wifiStarted) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiStarted = true;
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
  return true;
}

int postJson(const char* path, const String& body, bool useFactoryHeaders,
             const String& bearerToken, String& response) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const String url = String(BASE_URL) + path;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(10000);

  if (useFactoryHeaders) {
    http.addHeader("X-Nest-Factory-Id", FACTORY_ID);
    http.addHeader("X-Nest-Factory-Secret", FACTORY_SECRET);
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

int getJson(const String& path, String& response) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  const String url = String(BASE_URL) + path;
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(10000);

  const int status = http.GET();
  response = http.getString();
  http.end();
  return status;
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
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiStarted = true;
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

DevicePriceStatus fetchDevicePrice(uint8_t buttonNumber, String& priceText) {
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
  if (status == 401 || status == 403 || status == 404) {
    clearDeviceToken();
    return DevicePriceStatus::NeedsPairing;
  }
  if (status < 200 || status >= 300) {
    return DevicePriceStatus::Unavailable;
  }

  const String amount = extractJsonNumber(response, "amount");
  if (amount.length() == 0) {
    return DevicePriceStatus::Unavailable;
  }

  char formattedPrice[16];
  snprintf(formattedPrice, sizeof(formattedPrice), "$%.2f",
           amount.toFloat());
  priceText = formattedPrice;
  return DevicePriceStatus::Success;
}

bool submitDeviceOrder(uint8_t buttonNumber, String& orderId) {
  orderId = String();
  if (buttonNumber < 1 || buttonNumber > 4) {
    return false;
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
    return false;
  }

  const String token = loadDeviceToken();
  if (token.length() == 0) {
    return false;
  }

  const String eventId = nextEventId();
  const String body =
      "{\"button_number\":" + String(buttonNumber) +
      ",\"event_id\":\"" + eventId + "\"}";
  String response;
  const int status = postJson(ORDER_PATH, body, false, token, response);
  if (status < 200 || status >= 300) {
    return false;
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
  return orderId.length() > 0;
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
  status.deliveredAt = extractJsonScalar(response, "delivered_at");
  status.orderPhoto = extractJsonScalar(response, "order_photo");

  return status.orderStatus.length() > 0 ||
         status.orderNumber.length() > 0 ||
         status.orderPlacedAt.length() > 0 ||
         status.customerOrderPlacedAt.length() > 0 ||
         status.deliveredAt.length() > 0;
}

#include "network/desk_data.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include <cmath>
#include <cstdio>

#include "network/device_network.h"

void releaseDeskSpotifyArt(DeskSpotify& spotify);

namespace {
// Open-Meteo, keyless. Mechanicsburg, PA (40.2143, -77.0086).
constexpr char WEATHER_URL[] =
    "https://api.open-meteo.com/v1/forecast?latitude=40.2143&longitude=-77.0086"
    "&current=temperature_2m,weather_code,is_day"
    "&temperature_unit=fahrenheit&timezone=America%2FNew_York";
constexpr char MEALS_URL[] = "http://158.69.214.241:4050/api/meals";
constexpr char NEWS_URL[] =
    "https://news.google.com/rss?hl=en-US&gl=US&ceid=US:en";
constexpr char STOCK_URL_PREFIX[] =
    "https://query1.finance.yahoo.com/v8/finance/chart/";
constexpr char STOCK_URL_SUFFIX[] = "?interval=1d&range=5d";
constexpr char USER_AGENT[] =
    "FalconFlash-Nest contact@falconflash.co";
constexpr char SPOTIFY_PATH[] = "/nest/device/spotify/now-playing";
constexpr uint32_t WIFI_WAIT_MS = 10000;
constexpr uint32_t WEATHER_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_MS = 30UL * 1000UL;
constexpr uint32_t STOCKS_REFRESH_MS = 60UL * 1000UL;
constexpr uint32_t MEALS_RETRY_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t HEADLINES_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t SPOTIFY_REFRESH_MS = 4UL * 1000UL;
constexpr uint32_t HTTP_TIMEOUT_MS = 8000;
constexpr size_t SPOTIFY_ART_MAX_BYTES = 64 * 64 * 2;
bool clockStarted = false;

bool refreshDue(uint32_t lastAttempt, uint32_t intervalMs, bool force) {
  return force || lastAttempt == 0 || millis() - lastAttempt >= intervalMs;
}

bool waitForWiFi() {
  startDeviceWiFi();
  const uint32_t startedAt = millis();
  while (!deviceWiFiConnected() && millis() - startedAt < WIFI_WAIT_MS) {
    delay(50);
  }
  return deviceWiFiConnected();
}

String extractJsonScalar(const String& json, const char* key,
                         int searchFrom = 0) {
  const String token = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(token, searchFrom);
  if (keyStart < 0) {
    return String();
  }
  const int colon = json.indexOf(':', keyStart + token.length());
  if (colon < 0) {
    return String();
  }

  int valueStart = colon + 1;
  while (valueStart < json.length() && isspace(json[valueStart])) {
    ++valueStart;
  }
  if (valueStart >= json.length()) {
    return String();
  }

  if (json[valueStart] == '"') {
    String value;
    bool escaped = false;
    for (int index = valueStart + 1; index < json.length(); ++index) {
      const char character = json[index];
      if (escaped) {
        value += character == 'n' ? '\n' : character;
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        return value;
      } else {
        value += character;
      }
    }
    return String();
  }

  int valueEnd = valueStart;
  while (valueEnd < json.length() && json[valueEnd] != ',' &&
         json[valueEnd] != '}' && json[valueEnd] != ']') {
    ++valueEnd;
  }
  String value = json.substring(valueStart, valueEnd);
  value.trim();
  return value == "null" ? String() : value;
}

bool getSecureText(const String& url, String& response,
                   const char* accept = "application/json") {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", USER_AGENT);
  http.addHeader("Accept", accept);
  const int status = http.GET();
  if (status >= 200 && status < 300) {
    response = http.getString();
  }
  http.end();
  return status >= 200 && status < 300;
}

bool getPlainText(const String& url, String& response) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", USER_AGENT);
  const int status = http.GET();
  if (status >= 200 && status < 300) {
    response = http.getString();
  }
  http.end();
  return status >= 200 && status < 300;
}

// WMO weather interpretation codes used by Open-Meteo.
DeskWeatherIcon wmoIcon(int code) {
  if (code == 0 || code == 1) {
    return DeskWeatherIcon::Sun;
  }
  if (code == 2 || code == 3) {
    return DeskWeatherIcon::Cloud;
  }
  if (code == 45 || code == 48) {
    return DeskWeatherIcon::Fog;
  }
  if (code >= 51 && code <= 67) {
    return DeskWeatherIcon::Rain;
  }
  if (code >= 71 && code <= 77) {
    return DeskWeatherIcon::Snow;
  }
  if (code >= 80 && code <= 82) {
    return DeskWeatherIcon::Rain;
  }
  if (code >= 85 && code <= 86) {
    return DeskWeatherIcon::Snow;
  }
  if (code >= 95 && code <= 99) {
    return DeskWeatherIcon::Storm;
  }
  return DeskWeatherIcon::Unknown;
}

String wmoConditionText(int code) {
  switch (code) {
    case 0:
      return "Clear";
    case 1:
      return "Mainly clear";
    case 2:
      return "Partly cloudy";
    case 3:
      return "Overcast";
    case 45:
    case 48:
      return "Fog";
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
      return "Drizzle";
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
      return "Rain";
    case 71:
    case 73:
    case 75:
    case 77:
      return "Snow";
    case 80:
    case 81:
    case 82:
      return "Rain showers";
    case 85:
    case 86:
      return "Snow showers";
    case 95:
      return "Thunderstorm";
    case 96:
    case 99:
      return "Thunderstorm with hail";
    default:
      return "Conditions unavailable";
  }
}

bool isLocalNight() {
  const time_t now = time(nullptr);
  if (now < 100000) {
    return false;
  }
  struct tm localTime = {};
  localtime_r(&now, &localTime);
  return localTime.tm_hour < 6 || localTime.tm_hour >= 19;
}

bool fetchWeather(DeskWeather& weather) {
  String response;
  if (!getSecureText(WEATHER_URL, response)) {
    return false;
  }

  // Open-Meteo "current" block: temperature already in F, WMO code, is_day.
  const int currentStart = response.indexOf("\"current\"");
  if (currentStart < 0) {
    return false;
  }
  const String tempText =
      extractJsonScalar(response, "temperature_2m", currentStart);
  const String codeText =
      extractJsonScalar(response, "weather_code", currentStart);
  const String isDayText = extractJsonScalar(response, "is_day", currentStart);
  if (tempText.length() == 0 || codeText.length() == 0) {
    return false;
  }

  const int code = codeText.toInt();
  weather.valid = true;
  weather.hasTemperature = true;
  weather.temperatureF = static_cast<int16_t>(lroundf(tempText.toFloat()));
  weather.condition = wmoConditionText(code);
  weather.icon = wmoIcon(code);
  weather.isNight =
      isDayText.length() > 0 ? isDayText.toInt() == 0 : isLocalNight();
  return true;
}
bool fetchStock(DeskStock& stock) {
  String response;
  const String url =
      String(STOCK_URL_PREFIX) + stock.symbol + STOCK_URL_SUFFIX;
  if (!getSecureText(url, response)) {
    return false;
  }

  const String priceText = extractJsonScalar(response, "regularMarketPrice");
  const String changeText =
      extractJsonScalar(response, "regularMarketChangePercent");
  if (priceText.length() == 0 || changeText.length() == 0) {
    return false;
  }

  stock.price = priceText.toFloat();
  stock.changePercent = changeText.toFloat();
  stock.valid = true;
  return true;
}

bool fetchMeals(DeskMeals& meals) {
  String response;
  if (!getPlainText(MEALS_URL, response)) {
    return false;
  }

  const String breakfast = extractJsonScalar(response, "breakfast");
  const String lunch = extractJsonScalar(response, "lunch");
  const String dinner = extractJsonScalar(response, "dinner");
  if (lunch.length() == 0 && dinner.length() == 0) {
    return false;
  }

  meals.valid = true;
  meals.day = extractJsonScalar(response, "day");
  meals.breakfast = breakfast;
  meals.lunch = lunch;
  meals.dinner = dinner;
  return true;
}

String decodeXml(String text) {
  text.replace("&amp;", "&");
  text.replace("&quot;", "\"");
  text.replace("&#39;", "'");
  text.replace("&apos;", "'");
  text.replace("&lt;", "<");
  text.replace("&gt;", ">");
  text.trim();
  return text;
}

bool fetchHeadlines(DeskData& data) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, NEWS_URL)) {
    return false;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("User-Agent", USER_AGENT);
  http.addHeader("Accept", "application/rss+xml, application/xml, text/xml");
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  bool inItem = false;
  bool inTitle = false;
  String tag;
  String title;
  String headlines[DESK_HEADLINE_COUNT];
  uint8_t count = 0;
  uint32_t lastDataAt = millis();

  while ((http.connected() || stream->available()) &&
         count < DESK_HEADLINE_COUNT &&
         millis() - lastDataAt < HTTP_TIMEOUT_MS) {
    if (!stream->available()) {
      delay(1);
      continue;
    }
    const char character = static_cast<char>(stream->read());
    lastDataAt = millis();
    if (character == '<') {
      tag = "<";
    } else if (tag.length() > 0) {
      tag += character;
      if (tag.length() > 80) {
        tag = String();
      } else if (character == '>') {
        if (tag == "<item>") {
          inItem = true;
        } else if (inItem && tag == "<title>") {
          inTitle = true;
          title = String();
        } else if (inTitle && tag == "</title>") {
          inTitle = false;
          title = decodeXml(title);
          if (title.length() > 0) {
            headlines[count++] = title;
          }
        } else if (tag == "</item>") {
          inItem = false;
          inTitle = false;
        }
        tag = String();
      }
    } else if (inTitle && title.length() < 220) {
      title += character;
    }
  }
  http.end();

  if (count == 0) {
    return false;
  }
  data.headlineCount = count;
  for (uint8_t index = 0; index < count; ++index) {
    data.headlines[index] = headlines[index];
  }
  return true;
}

int8_t base64Value(char character) {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 26;
  }
  if (character >= '0' && character <= '9') {
    return character - '0' + 52;
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return -1;
}

bool decodeBase64Slice(const char* encoded, size_t encodedLen,
                       uint8_t*& output, size_t& outputLen) {
  output = nullptr;
  outputLen = 0;
  if (encoded == nullptr || encodedLen == 0) {
    return false;
  }

  const size_t capacity = (encodedLen / 4) * 3 + 3;
  if (capacity > SPOTIFY_ART_MAX_BYTES + 64) {
    return false;
  }
  uint8_t* buffer = static_cast<uint8_t*>(malloc(capacity));
  if (buffer == nullptr) {
    return false;
  }

  size_t outIndex = 0;
  int values[4];
  uint8_t valueCount = 0;
  for (size_t index = 0; index < encodedLen; ++index) {
    const char character = encoded[index];
    if (character == '=' || isspace(character)) {
      if (character == '=') {
        break;
      }
      continue;
    }
    const int8_t value = base64Value(character);
    if (value < 0) {
      free(buffer);
      return false;
    }
    values[valueCount++] = value;
    if (valueCount == 4) {
      buffer[outIndex++] = (values[0] << 2) | (values[1] >> 4);
      buffer[outIndex++] = ((values[1] & 0x0F) << 4) | (values[2] >> 2);
      buffer[outIndex++] = ((values[2] & 0x03) << 6) | values[3];
      valueCount = 0;
    }
  }
  if (valueCount == 2) {
    buffer[outIndex++] = (values[0] << 2) | (values[1] >> 4);
  } else if (valueCount == 3) {
    buffer[outIndex++] = (values[0] << 2) | (values[1] >> 4);
    buffer[outIndex++] = ((values[1] & 0x0F) << 4) | (values[2] >> 2);
  }

  output = buffer;
  outputLen = outIndex;
  return outIndex > 0;
}

bool findJsonStringRange(const String& json, const char* key, int searchFrom,
                         int& valueStart, int& valueLen) {
  valueStart = -1;
  valueLen = 0;
  const String token = "\"" + String(key) + "\"";
  const int keyStart = json.indexOf(token, searchFrom);
  if (keyStart < 0) {
    return false;
  }
  const int colon = json.indexOf(':', keyStart + token.length());
  if (colon < 0) {
    return false;
  }
  int cursor = colon + 1;
  while (cursor < json.length() && isspace(json[cursor])) {
    ++cursor;
  }
  if (cursor >= json.length() || json[cursor] != '"') {
    return false;
  }
  valueStart = cursor + 1;
  const int endQuote = json.indexOf('"', valueStart);
  if (endQuote < 0) {
    return false;
  }
  valueLen = endQuote - valueStart;
  return valueLen > 0;
}

bool jsonFlagIsTrue(const String& json, const char* key) {
  String value = extractJsonScalar(json, key);
  value.toLowerCase();
  return value == "true" || value == "1";
}

bool fetchSpotify(DeskSpotify& spotify) {
  String response;
  int httpStatus = 0;
  // Spotify auth failures must not unpair the device (clearTokenOnAuthFailure=false).
  const bool ok = fetchAuthenticatedJson(SPOTIFY_PATH, response, &httpStatus,
                                         false);
  const String linkedRaw = ok || response.length() > 0
                               ? extractJsonScalar(response, "linked")
                               : String();
  String linkedLower = linkedRaw;
  linkedLower.toLowerCase();
  String bodyLower = response;
  bodyLower.toLowerCase();
  // Account not linked: either an explicit linked=false or an error response
  // that says so. Show the "Link Spotify" prompt rather than stale/unavailable.
  const bool notLinked =
      linkedLower == "false" || linkedLower == "0" ||
      (!ok && (httpStatus == 404 || httpStatus == 409 || httpStatus == 412 ||
               httpStatus == 424) &&
       (bodyLower.indexOf("not linked") >= 0 ||
        bodyLower.indexOf("not_linked") >= 0 ||
        bodyLower.indexOf("not connected") >= 0 ||
        bodyLower.indexOf("no spotify") >= 0));
  if (notLinked) {
    releaseDeskSpotifyArt(spotify);
    spotify.valid = true;
    spotify.linked = false;
    spotify.isPlaying = false;
    spotify.progressMs = 0;
    spotify.durationMs = 0;
    spotify.emptyPolls = 0;
    spotify.track = "";
    spotify.artists = "";
    spotify.album = "";
    spotify.fetchedAt = millis() == 0 ? 1 : millis();
    return true;
  }
  if (!ok) {
    return false;
  }

  DeskSpotify next = spotify;
  next.valid = true;
  // Only trust an explicit linked value; missing/truncated JSON must not flash
  // "Link Spotify on Nest" while a session was already linked and playing.
  if (linkedRaw.length() > 0) {
    String linkedValue = linkedRaw;
    linkedValue.toLowerCase();
    next.linked = linkedValue == "true" || linkedValue == "1";
  } else {
    next.linked = spotify.linked;
  }
  next.isPlaying = jsonFlagIsTrue(response, "is_playing") ||
                   jsonFlagIsTrue(response, "playing");
  const String progressRaw = extractJsonScalar(response, "progress_ms");
  const String durationRaw = extractJsonScalar(response, "duration_ms");
  next.progressMs = progressRaw.toInt();
  next.durationMs = durationRaw.toInt();
  next.track = extractJsonScalar(response, "track");
  next.artists = extractJsonScalar(response, "artists");
  next.album = extractJsonScalar(response, "album");
  next.fetchedAt = millis() == 0 ? 1 : millis();

  // Same song but a missing/garbled timing field: keep the last known values
  // so the progress bar does not blink out for a poll.
  if (next.track.length() > 0 && next.track == spotify.track) {
    if (durationRaw.length() == 0 || next.durationMs <= 0) {
      next.durationMs = spotify.durationMs;
    }
    if (progressRaw.length() == 0) {
      uint32_t estimate = spotify.progressMs;
      if (spotify.isPlaying && spotify.fetchedAt > 0) {
        estimate += millis() - spotify.fetchedAt;
      }
      next.progressMs = estimate;
    }
  }
  if (next.track.length() > 0 || next.isPlaying || next.album.length() > 0) {
    next.linked = true;
  }

  // Between-track API gaps often return an empty item for a poll or two while
  // audio is already on the next song. Keep the previous title/art through a
  // short gap (or while the API still says playing), but after that fall back
  // to "Nothing playing".
  next.emptyPolls = 0;
  if (next.track.length() == 0 && spotify.track.length() > 0 &&
      (next.isPlaying || spotify.emptyPolls < 2)) {
    next.emptyPolls = next.isPlaying ? 0 : spotify.emptyPolls + 1;
    next.track = spotify.track;
    next.artists = spotify.artists;
    next.album = spotify.album;
    if (next.durationMs == 0) {
      next.durationMs = spotify.durationMs;
    }
    if (next.progressMs == 0 && next.isPlaying) {
      next.progressMs = spotify.progressMs;
    }
  }

  // Only clear when the API reports idle with no track — never wipe a still-
  // playing (or transitioning) response just because progress == duration.
  if (next.track.length() == 0 && !next.isPlaying) {
    next.progressMs = 0;
    next.durationMs = 0;
  } else if (next.durationMs > 0 && next.progressMs > next.durationMs) {
    next.progressMs = next.durationMs;
  }

  const bool hasTrack = next.track.length() > 0;
  const bool sameTrack =
      hasTrack && spotify.artPixels != nullptr && next.track == spotify.track;

  uint16_t* artPixels = nullptr;
  int16_t artWidth = 0;
  int16_t artHeight = 0;
  // Skip expensive base64 decode when we already have this track's cover —
  // decoding every poll can stall the UI for seconds.
  if (hasTrack && !sameTrack) {
    const int artStart = response.indexOf("\"album_art\"");
    if (artStart >= 0) {
      String format = extractJsonScalar(response, "format", artStart);
      format.toLowerCase();
      artWidth = extractJsonScalar(response, "width", artStart).toInt();
      artHeight = extractJsonScalar(response, "height", artStart).toInt();
      const int expectedBytes = artWidth * artHeight * 2;
      const int reportedBytes =
          extractJsonScalar(response, "bytes", artStart).toInt();
      int b64Start = -1;
      int b64Len = 0;
      uint8_t* raw = nullptr;
      size_t rawLen = 0;
      if (format == "rgb565" && artWidth > 0 && artHeight > 0 &&
          artWidth <= 64 && artHeight <= 64 && expectedBytes > 0 &&
          expectedBytes <= static_cast<int>(SPOTIFY_ART_MAX_BYTES) &&
          (reportedBytes == 0 || reportedBytes == expectedBytes) &&
          findJsonStringRange(response, "base64", artStart, b64Start, b64Len) &&
          decodeBase64Slice(response.c_str() + b64Start,
                            static_cast<size_t>(b64Len), raw, rawLen) &&
          rawLen >= static_cast<size_t>(expectedBytes)) {
        artPixels = reinterpret_cast<uint16_t*>(raw);
      } else if (raw != nullptr) {
        free(raw);
      }
    }
  }

  if (sameTrack) {
    next.artPixels = spotify.artPixels;
    next.artWidth = spotify.artWidth;
    next.artHeight = spotify.artHeight;
    spotify.artPixels = nullptr;
    spotify.artWidth = 0;
    spotify.artHeight = 0;
  } else if (artPixels != nullptr && hasTrack) {
    releaseDeskSpotifyArt(spotify);
    next.artPixels = artPixels;
    next.artWidth = artWidth;
    next.artHeight = artHeight;
  } else {
    // New track whose cover failed to decode: show the logo, never the old
    // song's cover.
    if (artPixels != nullptr) {
      free(artPixels);
    }
    next.artPixels = nullptr;
    next.artWidth = 0;
    next.artHeight = 0;
  }

  releaseDeskSpotifyArt(spotify);
  spotify = next;
  return true;
}
}  // namespace

void releaseDeskSpotifyArt(DeskSpotify& spotify) {
  if (spotify.artPixels != nullptr) {
    free(spotify.artPixels);
    spotify.artPixels = nullptr;
  }
  spotify.artWidth = 0;
  spotify.artHeight = 0;
}

namespace {
void releaseString(String& value) {
  // A null assignment is the call that frees a heap buffer. An empty
  // string keeps the old allocation.
  value = static_cast<const char*>(nullptr);
}
}  // namespace

void releaseDeskData(DeskData& data) {
  releaseDeskSpotifyArt(data.spotify);
  releaseString(data.weather.condition);
  data.weather.valid = false;
  data.weather.hasTemperature = false;
  releaseString(data.meals.day);
  releaseString(data.meals.breakfast);
  releaseString(data.meals.lunch);
  releaseString(data.meals.dinner);
  data.meals.valid = false;
  releaseString(data.spotify.track);
  releaseString(data.spotify.artists);
  releaseString(data.spotify.album);
  data.spotify.valid = false;
  data.spotify.linked = false;
  data.spotify.isPlaying = false;
  data.spotify.progressMs = 0;
  data.spotify.durationMs = 0;
  for (String& headline : data.headlines) releaseString(headline);
  data.headlineCount = 0;
  data.weatherFetchedAt = 0;
  data.stocksFetchedAt = 0;
  data.mealsFetchedAt = 0;
  data.headlinesFetchedAt = 0;
  data.spotifyFetchedAt = 0;
}

void initDeskClock() {
  if (clockStarted || !waitForWiFi()) {
    return;
  }
  configTime(-5 * 60 * 60, 0, "pool.ntp.org", "time.nist.gov");
  clockStarted = true;
}

bool formatDeskTime(char* timeText, size_t timeSize, char* dateText,
                    size_t dateSize) {
  const time_t now = time(nullptr);
  if (now < 100000) {
    return false;
  }
  struct tm localTime = {};
  localtime_r(&now, &localTime);
  const int displayHour =
      localTime.tm_hour % 12 == 0 ? 12 : localTime.tm_hour % 12;
  snprintf(timeText, timeSize, "%d:%02d %s", displayHour,
           localTime.tm_min, localTime.tm_hour >= 12 ? "PM" : "AM");
  static const char* WEEKDAYS[] = {"Sun", "Mon", "Tue", "Wed",
                                   "Thu", "Fri", "Sat"};
  static const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  snprintf(dateText, dateSize, "%s, %s %d",
           WEEKDAYS[localTime.tm_wday], MONTHS[localTime.tm_mon],
           localTime.tm_mday);
  return true;
}

void refreshSpotifyOnly(DeskData& data) {
  if (!fetchSpotify(data.spotify)) {
    // Keep title/art on a failed poll; only stop the progress bar if the
    // previous track has locally reached its end.
    if (data.spotify.isPlaying && data.spotify.durationMs > 0) {
      uint32_t progress = data.spotify.progressMs;
      if (data.spotify.fetchedAt > 0) {
        progress += millis() - data.spotify.fetchedAt;
      }
      if (progress >= data.spotify.durationMs) {
        data.spotify.isPlaying = false;
        data.spotify.progressMs = data.spotify.durationMs;
        data.spotifyFetchedAt = 0;
        return;
      }
    }
  }
  data.spotifyFetchedAt = millis() == 0 ? 1 : millis();
}

namespace {
// One stock quote per call so a full 1-minute market refresh never blocks the
// main loop for more than a single request.
uint8_t stockStep = 0;

uint32_t stampNow() { return millis() == 0 ? 1 : millis(); }

// Day key (year * 1000 + day-of-year) from the NTP clock; 0 if time unknown.
uint32_t currentDayKey() {
  const time_t now = time(nullptr);
  if (now < 100000) {
    return 0;
  }
  struct tm localTime = {};
  localtime_r(&now, &localTime);
  return static_cast<uint32_t>(localTime.tm_year + 1900) * 1000UL +
         static_cast<uint32_t>(localTime.tm_yday + 1);
}

void refreshWeatherNow(DeskData& data) {
  DeskWeather weather = data.weather;
  const bool ok = fetchWeather(weather);
  if (ok) {
    data.weather = weather;
    data.weatherFetchedAt = stampNow();
  } else {
    // Failed: retry in WEATHER_RETRY_MS instead of waiting a full interval.
    data.weatherFetchedAt =
        stampNow() - (WEATHER_REFRESH_MS - WEATHER_RETRY_MS);
  }
}

void refreshMealsNow(DeskData& data) {
  DeskMeals meals = data.meals;
  if (fetchMeals(meals)) {
    data.meals = meals;
    data.mealsDayKey = currentDayKey();
  }
  data.mealsFetchedAt = stampNow();
}

void refreshHeadlinesNow(DeskData& data) {
  fetchHeadlines(data);
  data.headlinesFetchedAt = stampNow();
}

void refreshStocksNow(DeskData& data) {
  for (uint8_t index = 0; index < DESK_STOCK_COUNT; ++index) {
    DeskStock stock = data.stocks[index];
    if (fetchStock(stock)) {
      data.stocks[index] = stock;
    }
  }
  data.stocksFetchedAt = stampNow();
  stockStep = 0;
}

void refreshStocksStep(DeskData& data) {
  const uint8_t index = stockStep;
  DeskStock stock = data.stocks[index];
  if (fetchStock(stock)) {
    data.stocks[index] = stock;
  }
  ++stockStep;
  if (stockStep >= DESK_STOCK_COUNT) {
    stockStep = 0;
    data.stocksFetchedAt = stampNow();
  }
}

bool mealsDue(const DeskData& data) {
  if (data.mealsFetchedAt == 0) {
    return true;
  }
  const uint32_t dayKey = currentDayKey();
  if (dayKey != 0 && dayKey != data.mealsDayKey) {
    // New day (or the last fetch failed): pull, but back off between retries.
    return refreshDue(data.mealsFetchedAt, MEALS_RETRY_MS, false);
  }
  return false;
}
}  // namespace

// Everything is polled from the main loop. Spotify every 4s, stocks every 1
// minute, weather every 5 minutes, headlines every 15 minutes, meals only on
// entering Desk Mode and when the day changes. Non-forced calls do at most one
// HTTP request so the caller can keep reading buttons between requests.
void refreshDeskData(DeskData& data, bool force) {
  if (!waitForWiFi()) {
    return;
  }
  initDeskClock();

  if (force) {
    stockStep = 0;
    refreshWeatherNow(data);
    refreshStocksNow(data);
    refreshMealsNow(data);
    refreshHeadlinesNow(data);
    refreshSpotifyOnly(data);
    return;
  }

  if (refreshDue(data.spotifyFetchedAt, SPOTIFY_REFRESH_MS, false)) {
    refreshSpotifyOnly(data);
    return;
  }
  if (stockStep != 0) {
    refreshStocksStep(data);
    return;
  }
  if (refreshDue(data.stocksFetchedAt, STOCKS_REFRESH_MS, false)) {
    refreshStocksStep(data);
    return;
  }
  if (refreshDue(data.weatherFetchedAt, WEATHER_REFRESH_MS, false)) {
    refreshWeatherNow(data);
    return;
  }
  if (mealsDue(data)) {
    refreshMealsNow(data);
    return;
  }
  if (refreshDue(data.headlinesFetchedAt, HEADLINES_REFRESH_MS, false)) {
    refreshHeadlinesNow(data);
  }
}

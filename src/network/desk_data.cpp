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
constexpr char WEATHER_URL[] =
    "https://api.weather.gov/stations/KCXY/observations/latest";
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
constexpr uint32_t WEATHER_STOCK_REFRESH_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t MEALS_REFRESH_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t HEADLINES_REFRESH_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t SPOTIFY_REFRESH_MS = 8UL * 1000UL;
constexpr uint32_t SPOTIFY_PLAYING_REFRESH_MS = 4UL * 1000UL;
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

DeskWeatherIcon weatherIconFor(String condition) {
  condition.toLowerCase();
  if (condition.indexOf("thunder") >= 0 ||
      condition.indexOf("storm") >= 0) {
    return DeskWeatherIcon::Storm;
  }
  if (condition.indexOf("snow") >= 0 ||
      condition.indexOf("sleet") >= 0 ||
      condition.indexOf("ice") >= 0) {
    return DeskWeatherIcon::Snow;
  }
  if (condition.indexOf("rain") >= 0 ||
      condition.indexOf("shower") >= 0 ||
      condition.indexOf("drizzle") >= 0) {
    return DeskWeatherIcon::Rain;
  }
  if (condition.indexOf("fog") >= 0 ||
      condition.indexOf("mist") >= 0 ||
      condition.indexOf("haze") >= 0) {
    return DeskWeatherIcon::Fog;
  }
  if (condition.indexOf("cloud") >= 0 ||
      condition.indexOf("overcast") >= 0) {
    return DeskWeatherIcon::Cloud;
  }
  if (condition.indexOf("clear") >= 0 ||
      condition.indexOf("sun") >= 0 ||
      condition.indexOf("fair") >= 0) {
    return DeskWeatherIcon::Sun;
  }
  return DeskWeatherIcon::Unknown;
}

bool fetchWeather(DeskWeather& weather) {
  String response;
  if (!getSecureText(WEATHER_URL, response)) {
    return false;
  }

  const String condition = extractJsonScalar(response, "textDescription");
  const int temperatureSection = response.indexOf("\"temperature\"");
  const String celsiusText =
      extractJsonScalar(response, "value", temperatureSection);
  if (condition.length() == 0 || celsiusText.length() == 0) {
    return false;
  }

  const float celsius = celsiusText.toFloat();
  weather.valid = true;
  weather.temperatureF =
      static_cast<int16_t>(lroundf(celsius * 9.0f / 5.0f + 32.0f));
  weather.condition = condition;
  weather.icon = weatherIconFor(condition);
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

bool decodeBase64(const String& encoded, uint8_t*& output, size_t& outputLen) {
  output = nullptr;
  outputLen = 0;
  if (encoded.length() == 0) {
    return false;
  }

  const size_t capacity = (encoded.length() / 4) * 3 + 3;
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
  for (uint16_t index = 0; index < encoded.length(); ++index) {
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

bool jsonFlagIsTrue(const String& json, const char* key) {
  String value = extractJsonScalar(json, key);
  value.toLowerCase();
  return value == "true" || value == "1";
}

bool fetchSpotify(DeskSpotify& spotify) {
  String response;
  if (!fetchAuthenticatedJson(SPOTIFY_PATH, response)) {
    return false;
  }

  DeskSpotify next = spotify;
  next.valid = true;
  // Only trust an explicit linked value; missing/truncated JSON must not flash
  // "Link Spotify on Nest" while a session was already linked and playing.
  const String linkedRaw = extractJsonScalar(response, "linked");
  if (linkedRaw.length() > 0) {
    String linkedValue = linkedRaw;
    linkedValue.toLowerCase();
    next.linked = linkedValue == "true" || linkedValue == "1";
  } else {
    next.linked = spotify.linked;
  }
  next.isPlaying = jsonFlagIsTrue(response, "is_playing") ||
                   jsonFlagIsTrue(response, "playing");
  next.progressMs = extractJsonScalar(response, "progress_ms").toInt();
  next.durationMs = extractJsonScalar(response, "duration_ms").toInt();
  next.track = extractJsonScalar(response, "track");
  next.artists = extractJsonScalar(response, "artists");
  next.album = extractJsonScalar(response, "album");
  next.fetchedAt = millis() == 0 ? 1 : millis();
  if (next.track.length() > 0 || next.isPlaying || next.album.length() > 0) {
    next.linked = true;
  }

  // Between-track API gaps often return an empty item for a poll or two while
  // audio is already on the next song. Keep the previous title/art instead of
  // flashing "Nothing playing".
  if (next.track.length() == 0 && spotify.track.length() > 0 &&
      (next.isPlaying || spotify.isPlaying)) {
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
      const String base64 = extractJsonScalar(response, "base64", artStart);
      uint8_t* raw = nullptr;
      size_t rawLen = 0;
      if (format == "rgb565" && artWidth > 0 && artHeight > 0 &&
          artWidth <= 64 && artHeight <= 64 && expectedBytes > 0 &&
          expectedBytes <= static_cast<int>(SPOTIFY_ART_MAX_BYTES) &&
          (reportedBytes == 0 || reportedBytes == expectedBytes) &&
          base64.length() > 0 && decodeBase64(base64, raw, rawLen) &&
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
  } else if (hasTrack && spotify.artPixels != nullptr) {
    next.artPixels = spotify.artPixels;
    next.artWidth = spotify.artWidth;
    next.artHeight = spotify.artHeight;
    spotify.artPixels = nullptr;
    spotify.artWidth = 0;
    spotify.artHeight = 0;
  } else {
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

void refreshWeatherStocks(DeskData& data) {
  DeskWeather weather = data.weather;
  if (fetchWeather(weather)) {
    data.weather = weather;
  }
  for (uint8_t index = 0; index < DESK_STOCK_COUNT; ++index) {
    DeskStock stock = data.stocks[index];
    if (fetchStock(stock)) {
      data.stocks[index] = stock;
    }
  }
  data.weatherStocksFetchedAt = millis() == 0 ? 1 : millis();
}

// Incremental market refresh so Desk Mode can poll buttons between requests.
uint8_t weatherStockStep = 0;

bool refreshWeatherStocksStep(DeskData& data) {
  if (weatherStockStep == 0) {
    DeskWeather weather = data.weather;
    if (fetchWeather(weather)) {
      data.weather = weather;
    }
    weatherStockStep = 1;
    return true;
  }
  if (weatherStockStep <= DESK_STOCK_COUNT) {
    const uint8_t index = weatherStockStep - 1;
    DeskStock stock = data.stocks[index];
    if (fetchStock(stock)) {
      data.stocks[index] = stock;
    }
    ++weatherStockStep;
    if (weatherStockStep > DESK_STOCK_COUNT) {
      weatherStockStep = 0;
      data.weatherStocksFetchedAt = millis() == 0 ? 1 : millis();
    }
    return true;
  }
  weatherStockStep = 0;
  return false;
}

void refreshDeskData(DeskData& data, bool force) {
  if (!waitForWiFi()) {
    return;
  }
  initDeskClock();

  const uint32_t spotifyInterval =
      data.spotify.isPlaying ? SPOTIFY_PLAYING_REFRESH_MS : SPOTIFY_REFRESH_MS;

  if (force) {
    weatherStockStep = 0;
    if (refreshDue(data.weatherStocksFetchedAt, WEATHER_STOCK_REFRESH_MS,
                   true)) {
      refreshWeatherStocks(data);
    }
    if (refreshDue(data.mealsFetchedAt, MEALS_REFRESH_MS, true)) {
      DeskMeals meals = data.meals;
      if (fetchMeals(meals)) {
        data.meals = meals;
      }
      data.mealsFetchedAt = millis() == 0 ? 1 : millis();
    }
    if (refreshDue(data.headlinesFetchedAt, HEADLINES_REFRESH_MS, true)) {
      fetchHeadlines(data);
      data.headlinesFetchedAt = millis() == 0 ? 1 : millis();
    }
    if (refreshDue(data.spotifyFetchedAt, spotifyInterval, true)) {
      refreshSpotifyOnly(data);
    }
    return;
  }

  // Background refresh: at most one HTTP feed per call so buttons stay
  // responsive between Spotify / weather / stock requests.
  if (weatherStockStep != 0) {
    refreshWeatherStocksStep(data);
    return;
  }
  if (refreshDue(data.spotifyFetchedAt, spotifyInterval, false)) {
    refreshSpotifyOnly(data);
    return;
  }
  if (refreshDue(data.weatherStocksFetchedAt, WEATHER_STOCK_REFRESH_MS,
                 false)) {
    refreshWeatherStocksStep(data);
    return;
  }
  if (refreshDue(data.mealsFetchedAt, MEALS_REFRESH_MS, false)) {
    DeskMeals meals = data.meals;
    if (fetchMeals(meals)) {
      data.meals = meals;
    }
    data.mealsFetchedAt = millis() == 0 ? 1 : millis();
    return;
  }
  if (refreshDue(data.headlinesFetchedAt, HEADLINES_REFRESH_MS, false)) {
    fetchHeadlines(data);
    data.headlinesFetchedAt = millis() == 0 ? 1 : millis();
  }
}

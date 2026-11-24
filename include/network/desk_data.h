#pragma once

#include <Arduino.h>
#include <stdint.h>

enum class DeskWeatherIcon : uint8_t {
  Unknown,
  Sun,
  Cloud,
  Rain,
  Snow,
  Fog,
  Storm,
};

struct DeskWeather {
  bool valid = false;
  int16_t temperatureF = 0;
  String condition;
  DeskWeatherIcon icon = DeskWeatherIcon::Unknown;
  bool isNight = false;
};

struct DeskStock {
  DeskStock(const char* stockSymbol = "") : symbol(stockSymbol) {}

  const char* symbol;
  bool valid = false;
  float price = 0.0f;
  float changePercent = 0.0f;
};

struct DeskMeals {
  bool valid = false;
  String day;
  String breakfast;
  String lunch;
  String dinner;
};

struct DeskSpotify {
  bool valid = false;
  bool linked = false;
  bool isPlaying = false;
  uint32_t progressMs = 0;
  uint32_t durationMs = 0;
  uint32_t fetchedAt = 0;
  // Consecutive polls with no track; used to tell a track gap from idle.
  uint8_t emptyPolls = 0;
  String track;
  String artists;
  String album;
  // Native RGB565 pixels ready for TFT_eSPI::pushImage.
  uint16_t* artPixels = nullptr;
  int16_t artWidth = 0;
  int16_t artHeight = 0;
};

constexpr uint8_t DESK_STOCK_COUNT = 4;
constexpr uint8_t DESK_HEADLINE_COUNT = 4;

struct DeskData {
  DeskWeather weather;
  DeskStock stocks[DESK_STOCK_COUNT] = {
      {"SPY"}, {"AAPL"}, {"NVDA"}, {"TSLA"}};
  DeskMeals meals;
  DeskSpotify spotify;
  String headlines[DESK_HEADLINE_COUNT];
  uint8_t headlineCount = 0;
  uint32_t weatherFetchedAt = 0;
  uint32_t stocksFetchedAt = 0;
  uint32_t mealsFetchedAt = 0;
  uint32_t mealsDayKey = 0;
  uint32_t headlinesFetchedAt = 0;
  uint32_t spotifyFetchedAt = 0;
};

void initDeskClock();
bool formatDeskTime(char* timeText, size_t timeSize, char* dateText,
                    size_t dateSize);
void releaseDeskSpotifyArt(DeskSpotify& spotify);
void refreshDeskData(DeskData& data, bool force = false);

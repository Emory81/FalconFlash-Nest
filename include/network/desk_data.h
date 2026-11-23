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

constexpr uint8_t DESK_STOCK_COUNT = 4;
constexpr uint8_t DESK_HEADLINE_COUNT = 4;

struct DeskData {
  DeskWeather weather;
  DeskStock stocks[DESK_STOCK_COUNT] = {
      {"SPY"}, {"AAPL"}, {"NVDA"}, {"TSLA"}};
  DeskMeals meals;
  String headlines[DESK_HEADLINE_COUNT];
  uint8_t headlineCount = 0;
  uint32_t weatherStocksFetchedAt = 0;
  uint32_t mealsFetchedAt = 0;
  uint32_t headlinesFetchedAt = 0;
};

void initDeskClock();
bool formatDeskTime(char* timeText, size_t timeSize, char* dateText,
                    size_t dateSize);
void refreshDeskData(DeskData& data, bool force = false);

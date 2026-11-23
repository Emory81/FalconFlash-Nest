#include "app/desk_mode.h"

#include <Arduino.h>

#include <cstdio>

#include "game_assets.h"
#include "input/button_input.h"
#include "network/desk_data.h"

namespace {
constexpr uint16_t DESK_BG = 0x1082;
constexpr uint16_t DESK_PANEL = 0x2104;
constexpr uint16_t DESK_ACCENT = 0x4D9F;
constexpr uint16_t DESK_SUBTEXT = 0xBDF7;
constexpr uint32_t PAGE_ROTATE_MS = 8000;
constexpr uint16_t FRAME_MS = 100;
constexpr uint8_t PAGE_COUNT = 4;

void drawHeader(TFT_eSPI& canvas, const char* title, uint8_t page,
                bool showHints) {
  canvas.fillRect(0, 0, canvas.width(), canvas.height(), DESK_BG);
  canvas.setTextFont(1);
  canvas.setTextSize(2);
  canvas.setTextColor(TFT_WHITE, DESK_BG);
  canvas.setTextDatum(TL_DATUM);
  canvas.drawString(title, 12, 9);

  for (uint8_t index = 0; index < PAGE_COUNT; ++index) {
    const int16_t x = canvas.width() - 54 + index * 12;
    canvas.fillCircle(x, 17, index == page ? 4 : 2,
                      index == page ? DESK_ACCENT : TFT_DARKGREY);
  }

  if (showHints) {
    canvas.setTextSize(1);
    canvas.setTextDatum(BC_DATUM);
    canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
    canvas.drawString("BTN1/2 Pages                         BTN4 Off",
                      canvas.width() / 2, canvas.height() - 5);
  }
}

uint8_t drawWrappedText(TFT_eSPI& canvas, const String& text, int16_t x,
                        int16_t y, int16_t width, uint8_t maxLines,
                        uint16_t color, uint16_t background) {
  String remaining = text;
  remaining.trim();
  const uint8_t calculatedChars = static_cast<uint8_t>(width / 6);
  const uint8_t charsPerLine =
      calculatedChars < 8 ? 8 : calculatedChars;
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextColor(color, background);

  uint8_t line = 0;
  while (remaining.length() > 0 && line < maxLines) {
    int splitAt = charsPerLine;
    if (remaining.length() <= charsPerLine) {
      splitAt = remaining.length();
    } else {
      const int space = remaining.lastIndexOf(' ', charsPerLine);
      if (space > 0) {
        splitAt = space;
      }
    }
    String lineText = remaining.substring(0, splitAt);
    lineText.trim();
    remaining = remaining.substring(splitAt);
    remaining.trim();
    if (line == maxLines - 1 && remaining.length() > 0) {
      lineText += "...";
    }
    canvas.drawString(lineText.c_str(), x, y + line * 12);
    ++line;
  }
  return line;
}

void drawSun(TFT_eSPI& canvas, int16_t x, int16_t y) {
  canvas.fillCircle(x, y, 17, TFT_YELLOW);
  for (uint8_t ray = 0; ray < 8; ++ray) {
    const float angle = ray * PI / 4.0f;
    canvas.drawLine(x + cosf(angle) * 22, y + sinf(angle) * 22,
                    x + cosf(angle) * 29, y + sinf(angle) * 29, TFT_YELLOW);
  }
}

void drawCloud(TFT_eSPI& canvas, int16_t x, int16_t y,
               uint16_t color = TFT_LIGHTGREY) {
  canvas.fillCircle(x - 13, y + 2, 13, color);
  canvas.fillCircle(x + 2, y - 7, 18, color);
  canvas.fillCircle(x + 18, y + 3, 12, color);
  canvas.fillRoundRect(x - 27, y + 1, 55, 19, 8, color);
}

void drawWeatherIcon(TFT_eSPI& canvas, DeskWeatherIcon icon, int16_t x,
                     int16_t y) {
  switch (icon) {
    case DeskWeatherIcon::Sun:
      drawSun(canvas, x, y);
      break;
    case DeskWeatherIcon::Rain:
      drawCloud(canvas, x, y - 7);
      for (int16_t offset = -16; offset <= 16; offset += 16) {
        canvas.drawLine(x + offset, y + 18, x + offset - 4, y + 29, TFT_CYAN);
      }
      break;
    case DeskWeatherIcon::Snow:
      drawCloud(canvas, x, y - 7);
      for (int16_t offset = -16; offset <= 16; offset += 16) {
        canvas.drawCircle(x + offset, y + 24, 3, TFT_WHITE);
      }
      break;
    case DeskWeatherIcon::Fog:
      drawCloud(canvas, x, y - 9);
      canvas.drawFastHLine(x - 28, y + 18, 56, TFT_LIGHTGREY);
      canvas.drawFastHLine(x - 23, y + 25, 46, TFT_LIGHTGREY);
      break;
    case DeskWeatherIcon::Storm:
      drawCloud(canvas, x, y - 8, TFT_DARKGREY);
      canvas.fillTriangle(x + 2, y + 10, x - 8, y + 28, x + 2, y + 25,
                          TFT_YELLOW);
      canvas.fillTriangle(x + 2, y + 25, x - 1, y + 39, x + 12, y + 20,
                          TFT_YELLOW);
      break;
    case DeskWeatherIcon::Cloud:
      drawCloud(canvas, x, y);
      break;
    default:
      canvas.setTextSize(4);
      canvas.setTextColor(TFT_WHITE, DESK_BG);
      canvas.setTextDatum(MC_DATUM);
      canvas.drawString("?", x, y);
      break;
  }
}

void drawClockWeather(TFT_eSPI& canvas, const DeskData& data,
                      bool showHints) {
  drawHeader(canvas, "Desk Mode", 0, showHints);

  char timeText[16] = "--:--";
  char dateText[24] = "Syncing time...";
  formatDeskTime(timeText, sizeof(timeText), dateText, sizeof(dateText));
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextSize(4);
  canvas.setTextColor(TFT_WHITE, DESK_BG);
  canvas.drawString(timeText, canvas.width() / 2, 64);
  canvas.setTextSize(1);
  canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
  canvas.drawString(dateText, canvas.width() / 2, 94);

  drawWeatherIcon(canvas, data.weather.icon, 82, 155);
  canvas.setTextDatum(ML_DATUM);
  if (data.weather.valid) {
    char temperature[16];
    snprintf(temperature, sizeof(temperature), "%d F",
             data.weather.temperatureF);
    canvas.setTextSize(3);
    canvas.setTextColor(TFT_WHITE, DESK_BG);
    canvas.drawString(temperature, 130, 140);
    canvas.setTextSize(1);
    canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
    drawWrappedText(canvas, data.weather.condition, 130, 164, 170, 2,
                    DESK_SUBTEXT, DESK_BG);
  } else {
    canvas.setTextSize(1);
    canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
    canvas.drawString("Weather unavailable", 130, 154);
  }
}

void drawStocks(TFT_eSPI& canvas, const DeskData& data, bool showHints) {
  drawHeader(canvas, "Markets", 1, showHints);
  for (uint8_t index = 0; index < DESK_STOCK_COUNT; ++index) {
    const int16_t column = index % 2;
    const int16_t row = index / 2;
    const int16_t x = 10 + column * 155;
    const int16_t y = 42 + row * 82;
    canvas.fillRoundRect(x, y, 145, 70, 8, DESK_PANEL);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(TFT_WHITE, DESK_PANEL);
    canvas.drawString(data.stocks[index].symbol, x + 10, y + 8);
    if (!data.stocks[index].valid) {
      canvas.setTextSize(1);
      canvas.setTextColor(DESK_SUBTEXT, DESK_PANEL);
      canvas.drawString("Unavailable", x + 10, y + 42);
      continue;
    }
    char price[20];
    char change[20];
    snprintf(price, sizeof(price), "$%.2f", data.stocks[index].price);
    snprintf(change, sizeof(change), "%+.2f%%",
             data.stocks[index].changePercent);
    canvas.setTextSize(1);
    canvas.setTextColor(TFT_WHITE, DESK_PANEL);
    canvas.drawString(price, x + 10, y + 39);
    canvas.setTextDatum(TR_DATUM);
    canvas.setTextColor(data.stocks[index].changePercent >= 0 ? TFT_GREEN
                                                              : TFT_RED,
                        DESK_PANEL);
    canvas.drawString(change, x + 135, y + 39);
  }
}

void drawMeals(TFT_eSPI& canvas, const DeskData& data, bool showHints) {
  drawHeader(canvas, "Lottie Meals", 2, showHints);
  if (!data.meals.valid) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
    canvas.drawString("Menu unavailable", canvas.width() / 2,
                      canvas.height() / 2);
    return;
  }

  canvas.setTextDatum(TC_DATUM);
  canvas.setTextSize(1);
  canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
  canvas.drawString(data.meals.day.c_str(), canvas.width() / 2, 35);

  String day = data.meals.day;
  day.toLowerCase();
  const bool weekend = day.indexOf("saturday") >= 0 ||
                       day.indexOf("sunday") >= 0;
  const uint8_t mealCount = weekend ? 2 : 3;
  const char* labels[] = {"Breakfast", "Lunch", "Dinner"};
  const String meals[] = {data.meals.breakfast, data.meals.lunch,
                          data.meals.dinner};
  const int16_t cardHeight = weekend ? 68 : 54;
  const int16_t cardGap = weekend ? 10 : 5;
  for (uint8_t index = weekend ? 1 : 0; index < 3; ++index) {
    const uint8_t cardIndex = weekend ? index - 1 : index;
    const int16_t y = 48 + cardIndex * (cardHeight + cardGap);
    canvas.fillRoundRect(12, y, canvas.width() - 24, cardHeight, 8,
                         DESK_PANEL);
    canvas.setTextDatum(TL_DATUM);
    canvas.setTextSize(1);
    canvas.setTextColor(DESK_ACCENT, DESK_PANEL);
    canvas.drawString(labels[index], 22, y + 8);
    drawWrappedText(canvas,
                    meals[index].length() > 0 ? meals[index] : "Not listed",
                    22, y + 26, canvas.width() - 44, weekend ? 2 : 1,
                    TFT_WHITE,
                    DESK_PANEL);
  }
}

void drawHeadlines(TFT_eSPI& canvas, const DeskData& data, bool showHints) {
  drawHeader(canvas, "Latest Headlines", 3, showHints);
  if (data.headlineCount == 0) {
    canvas.setTextDatum(MC_DATUM);
    canvas.setTextSize(2);
    canvas.setTextColor(DESK_SUBTEXT, DESK_BG);
    canvas.drawString("Headlines unavailable", canvas.width() / 2,
                      canvas.height() / 2);
    return;
  }

  int16_t y = 39;
  for (uint8_t index = 0;
       index < data.headlineCount && y < canvas.height() - 28; ++index) {
    canvas.fillCircle(12, y + 5, 3, DESK_ACCENT);
    const uint8_t lines =
        drawWrappedText(canvas, data.headlines[index], 23, y,
                        canvas.width() - 34, 3, TFT_WHITE, DESK_BG);
    y += lines * 12 + 8;
  }
}

void drawPage(TFT_eSPI& canvas, const DeskData& data, uint8_t page,
              bool showHints) {
  switch (page) {
    case 0:
      drawClockWeather(canvas, data, showHints);
      break;
    case 1:
      drawStocks(canvas, data, showHints);
      break;
    case 2:
      drawMeals(canvas, data, showHints);
      break;
    default:
      drawHeadlines(canvas, data, showHints);
      break;
  }
}

void drawLoading(TFT_eSPI& display) {
  display.fillScreen(DESK_BG);
  display.setTextFont(1);
  display.setTextDatum(MC_DATUM);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, DESK_BG);
  display.drawString("Loading Desk Mode", display.width() / 2,
                     display.height() / 2 - 8);
  display.setTextSize(1);
  display.setTextColor(DESK_SUBTEXT, DESK_BG);
  display.drawString("Getting weather, markets and news",
                     display.width() / 2, display.height() / 2 + 18);
}
}  // namespace

void runDeskMode(TFT_eSPI& display) {
  waitForButtonRelease();
  drawLoading(display);
  prepareGameAssetsForOrderRequest();

  DeskData data;
  initDeskClock();
  refreshDeskData(data, true);

  TFT_eSprite frame(&display);
  frame.setColorDepth(8);
  const bool useSprite =
      frame.createSprite(display.width(), display.height()) != nullptr;
  uint8_t page = 0;
  uint32_t nextPageAt = millis() + PAGE_ROTATE_MS;
  uint32_t nextFrameAt = 0;
  uint32_t lastInteractionAt = millis();
  bool showHints = true;

  while (true) {
    const Button button = readButtonEvent();
    if (button != Button::None) {
      lastInteractionAt = millis();
      showHints = true;
      nextFrameAt = 0;
    } else if (showHints && millis() - lastInteractionAt >= 3000) {
      showHints = false;
      nextFrameAt = 0;
    }
    if (button == Button::Button4) {
      waitForButtonRelease();
      frame.deleteSprite();
      return;
    }
    if (button == Button::Button1) {
      page = page == 0 ? PAGE_COUNT - 1 : page - 1;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      nextFrameAt = 0;
    } else if (button == Button::Button2) {
      page = (page + 1) % PAGE_COUNT;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      nextFrameAt = 0;
    }

    if (millis() >= nextPageAt) {
      page = (page + 1) % PAGE_COUNT;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      nextFrameAt = 0;
    }

    refreshDeskData(data);
    if (millis() >= nextFrameAt) {
      if (useSprite) {
        drawPage(frame, data, page, showHints);
        frame.pushSprite(0, 0);
      } else {
        drawPage(display, data, page, showHints);
      }
      nextFrameAt = millis() + FRAME_MS;
    }
    delay(10);
  }
}

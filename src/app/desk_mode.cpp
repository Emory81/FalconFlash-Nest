#include "app/desk_mode.h"

#include <Arduino.h>

#include <cstdio>
#include <cstring>

#include "game_assets.h"
#include "input/button_input.h"
#include "network/desk_data.h"

namespace {
constexpr uint16_t DESK_BG = 0x1082;
constexpr uint16_t DESK_PANEL = 0x2104;
constexpr uint16_t DESK_ACCENT = 0x4D9F;
constexpr uint16_t DESK_SUBTEXT = 0xBDF7;
constexpr uint16_t SPOTIFY_GREEN = 0x1DCA;  // #1DB954
constexpr uint16_t SPOTIFY_LOGO_TRANSPARENT = 0xF81F;
constexpr int16_t SPOTIFY_ART_RADIUS = 12;
constexpr uint32_t PAGE_ROTATE_MS = 8000;
constexpr uint16_t FRAME_MS = 100;
constexpr uint8_t PAGE_COUNT = 4;
constexpr int16_t SPOTIFY_PANEL_Y = 128;
constexpr int16_t SPOTIFY_PANEL_H = 88;
constexpr int16_t SPOTIFY_ART_SIZE = 64;
constexpr int16_t SPOTIFY_ART_X = 20;
constexpr int16_t SPOTIFY_ART_Y =
    SPOTIFY_PANEL_Y + (SPOTIFY_PANEL_H - SPOTIFY_ART_SIZE) / 2;
constexpr int16_t SPOTIFY_BAR_Y = SPOTIFY_PANEL_Y + 70;

uint32_t spotifyProgressMs(const DeskSpotify& spotify) {
  uint32_t progressMs = spotify.progressMs;
  if (spotify.isPlaying && spotify.fetchedAt > 0) {
    progressMs += millis() - spotify.fetchedAt;
  }
  if (spotify.durationMs > 0 && progressMs > spotify.durationMs) {
    progressMs = spotify.durationMs;
  }
  return progressMs;
}

void drawSpotifyProgress(TFT_eSPI& display, const DeskSpotify& spotify) {
  if (!spotify.valid || !spotify.linked || spotify.track.length() == 0 ||
      spotify.durationMs == 0) {
    return;
  }
  const int16_t barX = SPOTIFY_ART_X + SPOTIFY_ART_SIZE + 10;
  const int16_t barW = display.width() - barX - 20;
  const uint32_t progressMs = spotifyProgressMs(spotify);
  const int16_t fillW = static_cast<int16_t>(
      (static_cast<uint32_t>(barW) * progressMs) / spotify.durationMs);
  display.fillRoundRect(barX, SPOTIFY_BAR_Y, barW, 6, 3, TFT_DARKGREY);
  if (fillW > 0) {
    display.fillRoundRect(barX, SPOTIFY_BAR_Y, fillW, 6, 3, SPOTIFY_GREEN);
  }
}

bool spotifyHasCover(const DeskSpotify& spotify) {
  return spotify.valid && spotify.linked && spotify.isPlaying &&
         spotify.track.length() > 0 && spotify.artPixels != nullptr &&
         spotify.artWidth > 0 && spotify.artHeight > 0;
}

bool spotifyLogoReady(const GameAssetImage& logo) {
  return logo.data != nullptr && logo.width > 0 && logo.height > 0 &&
         logo.pixels() != nullptr;
}

void maskRoundedCorners(TFT_eSPI& display, int16_t x, int16_t y, int16_t w,
                        int16_t h, int16_t radius, uint16_t fill) {
  if (radius <= 0 || w <= 0 || h <= 0) {
    return;
  }
  if (radius * 2 > w) {
    radius = w / 2;
  }
  if (radius * 2 > h) {
    radius = h / 2;
  }
  const int32_t r = radius;
  const int32_t rSq = (r - 1) * (r - 1);
  for (int16_t dy = 0; dy < radius; ++dy) {
    for (int16_t dx = 0; dx < radius; ++dx) {
      const int32_t distSq =
          (r - 1 - dx) * (r - 1 - dx) + (r - 1 - dy) * (r - 1 - dy);
      if (distSq <= rSq) {
        continue;
      }
      display.drawPixel(x + dx, y + dy, fill);
      display.drawPixel(x + w - 1 - dx, y + dy, fill);
      display.drawPixel(x + dx, y + h - 1 - dy, fill);
      display.drawPixel(x + w - 1 - dx, y + h - 1 - dy, fill);
    }
  }
}

// RGB565 art must be blitted to the 16-bit display after the 8-bit sprite.
void pushSpotifyArtSlot(TFT_eSPI& display, const DeskSpotify& spotify,
                        const GameAssetImage& logo) {
  if (spotifyHasCover(spotify)) {
    const int16_t drawX =
        SPOTIFY_ART_X + (SPOTIFY_ART_SIZE - spotify.artWidth) / 2;
    const int16_t drawY =
        SPOTIFY_ART_Y + (SPOTIFY_ART_SIZE - spotify.artHeight) / 2;
    display.setSwapBytes(true);
    display.pushImage(drawX, drawY, spotify.artWidth, spotify.artHeight,
                      spotify.artPixels);
    display.setSwapBytes(false);
    maskRoundedCorners(display, drawX, drawY, spotify.artWidth,
                       spotify.artHeight, SPOTIFY_ART_RADIUS, DESK_PANEL);
    return;
  }

  if (!spotifyLogoReady(logo)) {
    return;
  }

  // Full logo tile with magenta-keyed rounded corners.
  const int16_t drawX =
      SPOTIFY_ART_X + (SPOTIFY_ART_SIZE - static_cast<int16_t>(logo.width)) / 2;
  const int16_t drawY =
      SPOTIFY_ART_Y +
      (SPOTIFY_ART_SIZE - static_cast<int16_t>(logo.height)) / 2;
  display.setSwapBytes(true);
  display.pushImage(drawX, drawY, logo.width, logo.height, logo.pixels(),
                    SPOTIFY_LOGO_TRANSPARENT);
  display.setSwapBytes(false);
}

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
  canvas.fillCircle(x, y, 11, TFT_YELLOW);
  for (uint8_t ray = 0; ray < 8; ++ray) {
    const float angle = ray * PI / 4.0f;
    canvas.drawLine(x + cosf(angle) * 14, y + sinf(angle) * 14,
                    x + cosf(angle) * 18, y + sinf(angle) * 18, TFT_YELLOW);
  }
}

void drawCloud(TFT_eSPI& canvas, int16_t x, int16_t y,
               uint16_t color = TFT_LIGHTGREY) {
  canvas.fillCircle(x - 8, y + 1, 8, color);
  canvas.fillCircle(x + 1, y - 4, 11, color);
  canvas.fillCircle(x + 11, y + 2, 7, color);
  canvas.fillRoundRect(x - 16, y + 1, 33, 11, 5, color);
}

void drawWeatherIcon(TFT_eSPI& canvas, DeskWeatherIcon icon, int16_t x,
                     int16_t y) {
  switch (icon) {
    case DeskWeatherIcon::Sun:
      drawSun(canvas, x, y);
      break;
    case DeskWeatherIcon::Rain:
      drawCloud(canvas, x, y - 4);
      for (int16_t offset = -10; offset <= 10; offset += 10) {
        canvas.drawLine(x + offset, y + 10, x + offset - 3, y + 18, TFT_CYAN);
      }
      break;
    case DeskWeatherIcon::Snow:
      drawCloud(canvas, x, y - 4);
      for (int16_t offset = -10; offset <= 10; offset += 10) {
        canvas.drawCircle(x + offset, y + 14, 2, TFT_WHITE);
      }
      break;
    case DeskWeatherIcon::Fog:
      drawCloud(canvas, x, y - 5);
      canvas.drawFastHLine(x - 16, y + 10, 32, TFT_LIGHTGREY);
      canvas.drawFastHLine(x - 12, y + 15, 24, TFT_LIGHTGREY);
      break;
    case DeskWeatherIcon::Storm:
      drawCloud(canvas, x, y - 5, TFT_DARKGREY);
      canvas.fillTriangle(x + 1, y + 6, x - 5, y + 16, x + 1, y + 14,
                          TFT_YELLOW);
      canvas.fillTriangle(x + 1, y + 14, x - 1, y + 22, x + 7, y + 11,
                          TFT_YELLOW);
      break;
    case DeskWeatherIcon::Cloud:
      drawCloud(canvas, x, y);
      break;
    default:
      canvas.setTextSize(2);
      canvas.setTextColor(TFT_WHITE, DESK_BG);
      canvas.setTextDatum(MC_DATUM);
      canvas.drawString("?", x, y);
      break;
  }
}

void drawSpotifyWidget(TFT_eSPI& canvas, const DeskSpotify& spotify,
                       const GameAssetImage& logo) {
  canvas.fillRoundRect(12, SPOTIFY_PANEL_Y, canvas.width() - 24,
                       SPOTIFY_PANEL_H, 8, DESK_PANEL);

  const int16_t textX = SPOTIFY_ART_X + SPOTIFY_ART_SIZE + 10;
  const int16_t textWidth = canvas.width() - textX - 20;
  canvas.setTextDatum(TL_DATUM);
  canvas.setTextSize(1);
  canvas.setTextColor(SPOTIFY_GREEN, DESK_PANEL);
  canvas.drawString("Now Playing", textX, SPOTIFY_PANEL_Y + 8);

  if (spotifyHasCover(spotify)) {
    // Match panel so the RGB565 cover blit does not flash a black square.
    canvas.fillRect(SPOTIFY_ART_X, SPOTIFY_ART_Y, SPOTIFY_ART_SIZE,
                    SPOTIFY_ART_SIZE, DESK_PANEL);
  } else if (spotifyLogoReady(logo)) {
    // Panel placeholder; full logo RGB565 is blitted after the 8-bit sprite.
    canvas.fillRect(SPOTIFY_ART_X, SPOTIFY_ART_Y, SPOTIFY_ART_SIZE,
                    SPOTIFY_ART_SIZE, DESK_PANEL);
  } else {
    canvas.fillRect(SPOTIFY_ART_X, SPOTIFY_ART_Y, SPOTIFY_ART_SIZE,
                    SPOTIFY_ART_SIZE, DESK_PANEL);
  }

  if (!spotify.valid) {
    canvas.setTextColor(DESK_SUBTEXT, DESK_PANEL);
    canvas.drawString("Spotify unavailable", textX, SPOTIFY_PANEL_Y + 34);
    return;
  }
  if (!spotify.linked) {
    canvas.setTextColor(DESK_SUBTEXT, DESK_PANEL);
    canvas.drawString("Link Spotify on Nest", textX, SPOTIFY_PANEL_Y + 34);
    return;
  }
  if (!spotify.isPlaying || spotify.track.length() == 0) {
    canvas.setTextColor(DESK_SUBTEXT, DESK_PANEL);
    canvas.drawString("Nothing playing", textX, SPOTIFY_PANEL_Y + 34);
    return;
  }

  canvas.setTextColor(TFT_WHITE, DESK_PANEL);
  drawWrappedText(canvas, spotify.track, textX, SPOTIFY_PANEL_Y + 24,
                  textWidth, 1, TFT_WHITE, DESK_PANEL);
  canvas.setTextColor(DESK_SUBTEXT, DESK_PANEL);
  drawWrappedText(canvas,
                  spotify.artists.length() > 0 ? spotify.artists
                                               : String("Unknown artist"),
                  textX, SPOTIFY_PANEL_Y + 40, textWidth, 1, DESK_SUBTEXT,
                  DESK_PANEL);

  const uint32_t progressMs = spotifyProgressMs(spotify);
  const int16_t barX = textX;
  const int16_t barW = textWidth;
  canvas.fillRoundRect(barX, SPOTIFY_BAR_Y, barW, 6, 3, TFT_DARKGREY);
  if (spotify.durationMs > 0) {
    const int16_t fillW = static_cast<int16_t>(
        (static_cast<uint32_t>(barW) * progressMs) / spotify.durationMs);
    if (fillW > 0) {
      canvas.fillRoundRect(barX, SPOTIFY_BAR_Y, fillW, 6, 3, SPOTIFY_GREEN);
    }
  }
}

// Match colors as seen after an 8-bit sprite push, so direct display patches
// do not flash a different background shade.
uint16_t spriteScreenColor(TFT_eSPI& display, uint16_t color) {
  return display.color8to16(display.color16to8(color));
}

void drawTimeBlock(TFT_eSPI& canvas, const char* timeText, const char* dateText,
                   uint16_t bg, uint16_t fg, uint16_t sub) {
  canvas.fillRect(0, 28, canvas.width(), 52, bg);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextSize(3);
  canvas.setTextColor(fg, bg);
  canvas.drawString(timeText, canvas.width() / 2, 42);
  canvas.setTextSize(1);
  canvas.setTextColor(sub, bg);
  canvas.drawString(dateText, canvas.width() / 2, 68);
}

void drawWeatherBlock(TFT_eSPI& canvas, const DeskWeather& weather, uint16_t bg,
                      uint16_t fg, uint16_t sub) {
  canvas.fillRect(0, 80, canvas.width(), SPOTIFY_PANEL_Y - 80, bg);
  drawWeatherIcon(canvas, weather.icon, 48, 98);
  canvas.setTextDatum(ML_DATUM);
  if (weather.valid) {
    char temperature[16];
    snprintf(temperature, sizeof(temperature), "%d F", weather.temperatureF);
    canvas.setTextSize(2);
    canvas.setTextColor(fg, bg);
    canvas.drawString(temperature, 78, 90);
    canvas.setTextSize(1);
    canvas.setTextColor(sub, bg);
    drawWrappedText(canvas, weather.condition, 78, 108, 220, 1, sub, bg);
  } else {
    canvas.setTextSize(1);
    canvas.setTextColor(sub, bg);
    canvas.drawString("Weather unavailable", 78, 98);
  }
}

void drawClockWeather(TFT_eSPI& canvas, const DeskData& data, bool showHints,
                      const GameAssetImage& spotifyLogo) {
  drawHeader(canvas, "Desk Mode", 0, showHints);
  char timeText[16] = "--:--";
  char dateText[24] = "Syncing time...";
  formatDeskTime(timeText, sizeof(timeText), dateText, sizeof(dateText));
  drawTimeBlock(canvas, timeText, dateText, DESK_BG, TFT_WHITE, DESK_SUBTEXT);
  drawWeatherBlock(canvas, data.weather, DESK_BG, TFT_WHITE, DESK_SUBTEXT);
  drawSpotifyWidget(canvas, data.spotify, spotifyLogo);
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
              bool showHints, const GameAssetImage& spotifyLogo) {
  switch (page) {
    case 0:
      drawClockWeather(canvas, data, showHints, spotifyLogo);
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

  GameAssetImage spotifyLogo;
  loadDeskSpotifyLogo(spotifyLogo);

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
  uint32_t nextFullRedrawAt = 0;
  uint32_t lastInteractionAt = millis();
  bool showHints = true;
  bool pageDirty = true;
  bool spotifyArtDirty = true;
  String lastSpotifyTrack;
  String lastSpotifyArtists;
  bool lastSpotifyPlaying = false;
  bool lastSpotifyLinked = false;
  bool lastSpotifyValid = false;
  const uint16_t* lastDrawnArt = nullptr;
  char lastTimeText[16] = "";
  char lastDateText[24] = "";
  bool lastWeatherValid = false;
  int16_t lastWeatherTemp = 0;
  DeskWeatherIcon lastWeatherIcon = DeskWeatherIcon::Unknown;
  String lastWeatherCondition;

  while (true) {
    const Button button = readButtonEvent();
    if (button != Button::None) {
      lastInteractionAt = millis();
      showHints = true;
      pageDirty = true;
    } else if (showHints && millis() - lastInteractionAt >= 3000) {
      showHints = false;
      pageDirty = true;
    }
    if (button == Button::Button4) {
      waitForButtonRelease();
      frame.deleteSprite();
      spotifyLogo.release();
      releaseDeskSpotifyArt(data.spotify);
      return;
    }
    if (button == Button::Button1) {
      page = page == 0 ? PAGE_COUNT - 1 : page - 1;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      pageDirty = true;
      spotifyArtDirty = true;
    } else if (button == Button::Button2) {
      page = (page + 1) % PAGE_COUNT;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      pageDirty = true;
      spotifyArtDirty = true;
    }

    if (millis() >= nextPageAt) {
      page = (page + 1) % PAGE_COUNT;
      nextPageAt = millis() + PAGE_ROTATE_MS;
      pageDirty = true;
      spotifyArtDirty = true;
    }

    refreshDeskData(data);
    if (data.spotify.track != lastSpotifyTrack ||
        data.spotify.artists != lastSpotifyArtists ||
        data.spotify.isPlaying != lastSpotifyPlaying ||
        data.spotify.linked != lastSpotifyLinked ||
        data.spotify.valid != lastSpotifyValid) {
      pageDirty = true;
      lastSpotifyTrack = data.spotify.track;
      lastSpotifyArtists = data.spotify.artists;
      lastSpotifyPlaying = data.spotify.isPlaying;
      lastSpotifyLinked = data.spotify.linked;
      lastSpotifyValid = data.spotify.valid;
    }
    const uint16_t* desiredArt =
        spotifyHasCover(data.spotify)
            ? data.spotify.artPixels
            : (spotifyLogoReady(spotifyLogo) ? spotifyLogo.pixels() : nullptr);
    if (desiredArt != lastDrawnArt) {
      spotifyArtDirty = true;
    }

    const bool needsFullRedraw =
        pageDirty || (page != 0 && millis() >= nextFullRedrawAt);
    if (millis() >= nextFrameAt) {
      if (needsFullRedraw) {
        if (useSprite) {
          drawPage(frame, data, page, showHints, spotifyLogo);
          frame.pushSprite(0, 0);
        } else {
          drawPage(display, data, page, showHints, spotifyLogo);
        }
        if (page == 0) {
          // Full page redraw clears the art slot; restore once.
          pushSpotifyArtSlot(display, data.spotify, spotifyLogo);
          lastDrawnArt = desiredArt;
          spotifyArtDirty = false;
          formatDeskTime(lastTimeText, sizeof(lastTimeText), lastDateText,
                         sizeof(lastDateText));
          lastWeatherValid = data.weather.valid;
          lastWeatherTemp = data.weather.temperatureF;
          lastWeatherIcon = data.weather.icon;
          lastWeatherCondition = data.weather.condition;
        }
        pageDirty = false;
        nextFullRedrawAt = millis() + FRAME_MS;
      } else if (page == 0) {
        char timeText[16] = "--:--";
        char dateText[24] = "Syncing time...";
        formatDeskTime(timeText, sizeof(timeText), dateText, sizeof(dateText));
        const uint16_t patchBg =
            useSprite ? spriteScreenColor(display, DESK_BG) : DESK_BG;
        const uint16_t patchFg =
            useSprite ? spriteScreenColor(display, TFT_WHITE) : TFT_WHITE;
        const uint16_t patchSub =
            useSprite ? spriteScreenColor(display, DESK_SUBTEXT) : DESK_SUBTEXT;
        if (strcmp(timeText, lastTimeText) != 0 ||
            strcmp(dateText, lastDateText) != 0) {
          drawTimeBlock(display, timeText, dateText, patchBg, patchFg,
                        patchSub);
          strncpy(lastTimeText, timeText, sizeof(lastTimeText) - 1);
          lastTimeText[sizeof(lastTimeText) - 1] = '\0';
          strncpy(lastDateText, dateText, sizeof(lastDateText) - 1);
          lastDateText[sizeof(lastDateText) - 1] = '\0';
        }
        if (data.weather.valid != lastWeatherValid ||
            data.weather.temperatureF != lastWeatherTemp ||
            data.weather.icon != lastWeatherIcon ||
            data.weather.condition != lastWeatherCondition) {
          drawWeatherBlock(display, data.weather, patchBg, patchFg, patchSub);
          lastWeatherValid = data.weather.valid;
          lastWeatherTemp = data.weather.temperatureF;
          lastWeatherIcon = data.weather.icon;
          lastWeatherCondition = data.weather.condition;
        }
        if (spotifyArtDirty) {
          pushSpotifyArtSlot(display, data.spotify, spotifyLogo);
          lastDrawnArt = desiredArt;
          spotifyArtDirty = false;
        }
        drawSpotifyProgress(display, data.spotify);
      }
      nextFrameAt = millis() + FRAME_MS;
    }
    delay(10);
  }
}

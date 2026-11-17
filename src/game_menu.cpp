#include "game_menu.h"

#include <Arduino.h>

#include "button_input.h"
#include "flappy_bird.h"
#include "game_assets.h"
#include "guitar_hero.h"
#include "super_flashy_birds.h"

namespace {
constexpr uint16_t MENU_BG = 0x18C7;
constexpr uint16_t MENU_TILE = TFT_BLACK;
constexpr int16_t TILE_WIDTH = 96;
constexpr int16_t TILE_HEIGHT = 84;
constexpr int16_t TILE_RADIUS = 8;
constexpr int16_t TILE_GAP = 8;
constexpr int16_t TILE_LEFT = 8;
constexpr int16_t TILE_TOP = 36;
constexpr int16_t BACKGROUND_WIDTH = 320;
constexpr int16_t BACKGROUND_HEIGHT = 240;
constexpr uint16_t SPRITE_TRANSPARENT = 0xF81F;

constexpr const char* GAME_NAMES[] = {
    "Flappy Falcon", "Guitar Hero", "Pong", "Brick Breaker", "Runner",
    "Flashy Birds"};

int16_t tileX(uint8_t index) {
  return TILE_LEFT + (index % 3) * (TILE_WIDTH + TILE_GAP);
}

int16_t tileY(uint8_t index) {
  return TILE_TOP + (index / 3) * (TILE_HEIGHT + TILE_GAP);
}

void drawGuitarIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRoundRect(x + 27, y + 3, 14, 42, 4, TFT_DARKGREY);
  display.drawRoundRect(x + 27, y + 3, 14, 42, 4, TFT_WHITE);
  display.fillCircle(x + 34, y + 42, 12, TFT_RED);
  display.drawCircle(x + 34, y + 42, 12, TFT_WHITE);
  display.drawFastVLine(x + 31, y + 8, 31, TFT_WHITE);
  display.drawFastVLine(x + 37, y + 8, 31, TFT_WHITE);
  display.drawFastHLine(x + 28, y + 14, 12, TFT_YELLOW);
  display.drawFastHLine(x + 28, y + 23, 12, TFT_YELLOW);
  display.fillCircle(x + 57, y + 12, 4, TFT_YELLOW);
  display.fillCircle(x + 67, y + 28, 4, TFT_CYAN);
}

void drawPongIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRect(x + 10, y + 8, 5, 30, TFT_WHITE);
  display.fillRect(x + 61, y + 8, 5, 30, TFT_WHITE);
  display.fillCircle(x + 37, y + 22, 5, TFT_YELLOW);
}

void drawBrickBreakerIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRect(x + 10, y + 7, 20, 7, TFT_RED);
  display.fillRect(x + 33, y + 7, 20, 7, TFT_YELLOW);
  display.fillRect(x + 21, y + 17, 20, 7, TFT_GREEN);
  display.fillCircle(x + 39, y + 31, 4, TFT_WHITE);
  display.fillRoundRect(x + 25, y + 38, 28, 5, 2, TFT_BLUE);
}

void drawRunnerIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillCircle(x + 37, y + 8, 5, TFT_YELLOW);
  display.drawLine(x + 37, y + 13, x + 31, y + 26, TFT_WHITE);
  display.drawLine(x + 31, y + 26, x + 20, y + 37, TFT_WHITE);
  display.drawLine(x + 31, y + 26, x + 45, y + 37, TFT_WHITE);
  display.drawLine(x + 34, y + 18, x + 51, y + 18, TFT_WHITE);
  display.drawLine(x + 8, y + 41, x + 65, y + 41, TFT_GREEN);
}

void drawGameIcon(TFT_eSPI& display, uint8_t index, int16_t x, int16_t y,
                  const SuperFlashyAssets* superAssets) {
  switch (index) {
    case 1:
      drawGuitarIcon(display, x, y);
      break;
    case 2:
      drawPongIcon(display, x, y);
      break;
    case 3:
      drawBrickBreakerIcon(display, x, y);
      break;
    case 4:
      drawRunnerIcon(display, x, y);
      break;
    default:
      if (superAssets != nullptr) {
        drawSuperFlashyBirdsIcon(display, *superAssets, x + 8, y);
      } else {
        drawPongIcon(display, x, y);
      }
      break;
  }
}

void drawScaledFlappySprite(TFT_eSPI& display, int16_t x, int16_t y,
                            int16_t size, const FlappyAssets& assets) {
  for (int16_t destY = 0; destY < size; ++destY) {
    const int16_t sourceY =
        destY * assets.birdBack.height / size;
    for (int16_t destX = 0; destX < size; ++destX) {
      const int16_t sourceX =
          destX * assets.birdBack.width / size;
      const uint16_t color =
          assets.birdBack.pixels()[sourceY * assets.birdBack.width + sourceX];
      if (color != SPRITE_TRANSPARENT) {
        display.drawPixel(x + destX, y + destY, color);
      }
    }
  }
}

void maskFlappyTileCorners(TFT_eSPI& display, int16_t x, int16_t y) {
  for (int16_t cornerY = 0; cornerY < TILE_RADIUS; ++cornerY) {
    for (int16_t cornerX = 0; cornerX < TILE_RADIUS; ++cornerX) {
      const int16_t distanceX = TILE_RADIUS - 1 - cornerX;
      const int16_t distanceY = TILE_RADIUS - 1 - cornerY;
      if (distanceX * distanceX + distanceY * distanceY <
          TILE_RADIUS * TILE_RADIUS) {
        continue;
      }

      display.drawPixel(x + cornerX, y + cornerY, MENU_BG);
      display.drawPixel(x + TILE_WIDTH - 1 - cornerX, y + cornerY,
                        MENU_BG);
      display.drawPixel(x + cornerX, y + TILE_HEIGHT - 1 - cornerY,
                        MENU_BG);
      display.drawPixel(x + TILE_WIDTH - 1 - cornerX,
                        y + TILE_HEIGHT - 1 - cornerY, MENU_BG);
    }
  }
}

void drawFlappyTile(TFT_eSPI& display, int16_t x, int16_t y,
                    bool selected, const FlappyAssets* assets) {
  if (assets == nullptr) {
    display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          MENU_TILE);
    display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          selected ? TFT_YELLOW : TFT_WHITE);
    display.setTextFont(1);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, MENU_TILE);
    display.setTextDatum(MC_DATUM);
    display.drawString("Flappy Falcon", x + TILE_WIDTH / 2,
                       y + TILE_HEIGHT / 2);
    return;
  }
  TFT_eSprite tile(&display);
  tile.setColorDepth(8);
  if (!tile.createSprite(TILE_WIDTH, TILE_HEIGHT)) {
    display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          MENU_TILE);
    display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          selected ? TFT_YELLOW : TFT_WHITE);
    return;
  }

  uint8_t* const pixels = static_cast<uint8_t*>(tile.getPointer());
  const int16_t scaledWidth =
      BACKGROUND_WIDTH * TILE_HEIGHT / BACKGROUND_HEIGHT;
  const int16_t cropX = (scaledWidth - TILE_WIDTH) / 2;
  for (int16_t destY = 0; destY < TILE_HEIGHT; ++destY) {
    const int16_t sourceY =
        destY * BACKGROUND_HEIGHT / TILE_HEIGHT;
    for (int16_t destX = 0; destX < TILE_WIDTH; ++destX) {
      const int16_t scaledX = destX + cropX;
      const int16_t sourceX =
          scaledX * BACKGROUND_WIDTH / scaledWidth;
      pixels[destY * TILE_WIDTH + destX] =
          assets->backgroundDay.data[sourceY * BACKGROUND_WIDTH + sourceX];
    }
  }

  tile.setTextFont(1);
  tile.setTextSize(1);
  const int16_t labelBaseline = TILE_HEIGHT - 7;
  const int16_t labelTop = labelBaseline - tile.fontHeight();
  const int16_t iconSize = static_cast<int16_t>(TILE_HEIGHT * 0.6f);
  const int16_t iconY = (labelTop - iconSize) / 2;
  drawScaledFlappySprite(tile, (TILE_WIDTH - iconSize) / 2, iconY, iconSize,
                         *assets);

  tile.setTextDatum(BC_DATUM);
  tile.setTextColor(TFT_BLACK);
  tile.drawString("Flappy Falcon", TILE_WIDTH / 2 + 1, labelBaseline + 1);
  tile.setTextColor(TFT_WHITE);
  tile.drawString("Flappy Falcon", TILE_WIDTH / 2, labelBaseline);

  tile.drawRoundRect(0, 0, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                     selected ? TFT_YELLOW : TFT_WHITE);
  if (selected) {
    tile.drawRoundRect(2, 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                       TILE_RADIUS - 2, TFT_YELLOW);
  }
  tile.pushSprite(x, y);
  tile.deleteSprite();
  maskFlappyTileCorners(display, x, y);
  display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                        selected ? TFT_YELLOW : TFT_WHITE);
  if (selected) {
    display.drawRoundRect(x + 2, y + 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                          TILE_RADIUS - 2, TFT_YELLOW);
  }
}

void drawMenu(TFT_eSPI& display, uint8_t selectedIndex) {
  FlappyAssets flappyAssets;
  const bool hasFlappyAssets = loadFlappyAssets(flappyAssets);
  SuperFlashyAssets superAssets;
  const bool hasSuperAssets = loadSuperFlashyAssets(superAssets);

  display.fillScreen(MENU_BG);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(TC_DATUM);
  display.drawString("Games", display.width() / 2, 6);

  for (uint8_t index = 0; index < 6; ++index) {
    const int16_t x = tileX(index);
    const int16_t y = tileY(index);
    if (index == 0) {
      drawFlappyTile(display, x, y, selectedIndex == index,
                     hasFlappyAssets ? &flappyAssets : nullptr);
      continue;
    }

    display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          MENU_TILE);
    display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          index == selectedIndex ? TFT_YELLOW : TFT_WHITE);
    if (index == selectedIndex) {
      display.drawRoundRect(x + 2, y + 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                            TILE_RADIUS - 2, TFT_YELLOW);
    }
    drawGameIcon(display, index, x, y + 8,
                 hasSuperAssets ? &superAssets : nullptr);

    display.setTextFont(1);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, MENU_TILE);
    display.setTextDatum(BC_DATUM);
    display.drawString(GAME_NAMES[index], x + TILE_WIDTH / 2,
                       y + TILE_HEIGHT - 7);
  }

  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(BC_DATUM);
  display.drawString("BTN1/2 Move   BTN3 Select   BTN4 Back",
                     display.width() / 2,
                     display.height() - 4);
  flappyAssets.release();
  superAssets.release();
}

void showComingSoon(TFT_eSPI& display, const char* name) {
  display.fillScreen(MENU_BG);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(MC_DATUM);
  display.drawString(name, display.width() / 2, display.height() / 2 - 15);
  display.setTextFont(1);
  display.setTextSize(1);
  display.drawString("Coming soon", display.width() / 2,
                     display.height() / 2 + 20);
  delay(900);
}

void launchSelectedGame(TFT_eSPI& display, uint8_t selectedIndex) {
  if (selectedIndex == 0) {
    runFlappyBird(display);
  } else if (selectedIndex == 1) {
    runGuitarHero(display);
  } else if (selectedIndex == 5) {
    runSuperFlashyBirds(display);
  } else {
    showComingSoon(display, GAME_NAMES[selectedIndex]);
  }
}
}  // namespace

bool showGameMenu(TFT_eSPI& display, bool gameModeOnly) {
  uint8_t selectedIndex = 0;
  waitForButtonRelease();
  drawMenu(display, selectedIndex);

  while (true) {
    const Button button = readButtonEvent();
    if (button == Button::Button1) {
      selectedIndex = selectedIndex == 0 ? 5 : selectedIndex - 1;
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button2) {
      selectedIndex = (selectedIndex + 1) % 6;
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button3) {
      waitForButtonRelease();
      launchSelectedGame(display, selectedIndex);
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button4) {
      waitForButtonRelease();
      return gameModeOnly;
    }
    delay(10);
  }
}

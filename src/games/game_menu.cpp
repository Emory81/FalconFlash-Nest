#include "games/game_menu.h"

#include <Arduino.h>

#include "games/flappy_bird.h"
#include "games/guitar_hero.h"
#include "input/button_input.h"

namespace {
constexpr uint16_t MENU_BG = 0x18C7;
constexpr uint16_t MENU_TILE = TFT_BLACK;
constexpr int16_t TILE_WIDTH = 96;
constexpr int16_t TILE_HEIGHT = 84;
constexpr int16_t TILE_RADIUS = 8;
constexpr int16_t TILE_GAP = 8;
constexpr int16_t TILE_LEFT = 8;
constexpr int16_t TILE_TOP = 36;

constexpr const char* GAME_NAMES[] = {
    "Flappy Bird", "Guitar Hero", "Pong", "Brick Breaker", "Runner",
    "Memory"};

int16_t tileX(uint8_t index) {
  return TILE_LEFT + (index % 3) * (TILE_WIDTH + TILE_GAP);
}

int16_t tileY(uint8_t index) {
  return TILE_TOP + (index / 3) * (TILE_HEIGHT + TILE_GAP);
}

void drawFlappyIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRect(x + 8, y + 2, 10, 26, TFT_GREEN);
  display.fillRect(x + 58, y + 20, 10, 20, TFT_GREEN);
  display.fillCircle(x + 38, y + 22, 9, TFT_YELLOW);
  display.fillCircle(x + 42, y + 19, 2, TFT_BLACK);
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

void drawMemoryIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.drawRoundRect(x + 13, y + 7, 20, 30, 4, TFT_CYAN);
  display.drawRoundRect(x + 43, y + 7, 20, 30, 4, TFT_CYAN);
  display.drawLine(x + 19, y + 14, x + 27, y + 30, TFT_CYAN);
  display.drawLine(x + 27, y + 14, x + 19, y + 30, TFT_CYAN);
  display.fillCircle(x + 53, y + 22, 5, TFT_MAGENTA);
}

void drawGameIcon(TFT_eSPI& display, uint8_t index, int16_t x, int16_t y) {
  switch (index) {
    case 0:
      drawFlappyIcon(display, x, y);
      break;
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
      drawMemoryIcon(display, x, y);
      break;
  }
}

void drawMenu(TFT_eSPI& display, uint8_t selectedIndex) {
  display.fillScreen(MENU_BG);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(TC_DATUM);
  display.drawString("Games", display.width() / 2, 6);

  for (uint8_t index = 0; index < 6; ++index) {
    const int16_t x = tileX(index);
    const int16_t y = tileY(index);
    display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          MENU_TILE);
    display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          index == selectedIndex ? TFT_YELLOW : TFT_WHITE);
    if (index == selectedIndex) {
      display.drawRoundRect(x + 2, y + 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                            TILE_RADIUS - 2, TFT_YELLOW);
    }
    drawGameIcon(display, index, x, y + 8);

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

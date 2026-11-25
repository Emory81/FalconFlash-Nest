#include "game_menu.h"

#include <Arduino.h>

#include "arcade_games.h"
#include "button_input.h"
#include "flappy_bird.h"
#include "flappy_sprites.h"
#include "gallery.h"
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

// Three rows of three. The screen shows two rows and scrolls to the third.
constexpr const char* GAME_NAMES[] = {
    "Flappy Falcon", "Flashy Birds", "Gallery",
    "Tic Tac Toe",   "Rock Paper",   "Runner",
    "Brick Breaker", "Guitar Hero",  "Pong"};
constexpr uint8_t GAME_COUNT = sizeof(GAME_NAMES) / sizeof(GAME_NAMES[0]);
constexpr uint8_t VISIBLE_ROWS = 2;

// BTN2 switches modes. Two-player mode lists only the games that have it.
constexpr uint8_t TWO_PLAYER_GAMES[] = {3, 4, 8};
bool twoPlayer = false;

uint8_t listedCount() {
  return twoPlayer ? sizeof(TWO_PLAYER_GAMES) : GAME_COUNT;
}
// The game shown in place `slot` of the menu.
uint8_t gameAt(uint8_t slot) {
  return twoPlayer ? TWO_PLAYER_GAMES[slot] : slot;
}

uint8_t topRow = 0;  // first row on screen

// Releasing BTN3 sooner than this selects; holding it on shuts the device off.
constexpr uint32_t SELECT_MAX_MS = 600;
constexpr uint32_t SHUTDOWN_HOLD_MS = 3000;

int16_t tileX(uint8_t index) {
  return TILE_LEFT + (index % 3) * (TILE_WIDTH + TILE_GAP);
}

int16_t tileY(uint8_t index) {
  return TILE_TOP + (index / 3 - topRow) * (TILE_HEIGHT + TILE_GAP);
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

void drawBrickBreakerIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRect(x + 10, y + 7, 20, 7, TFT_RED);
  display.fillRect(x + 33, y + 7, 20, 7, TFT_YELLOW);
  display.fillRect(x + 21, y + 17, 20, 7, TFT_GREEN);
  display.fillCircle(x + 39, y + 31, 4, TFT_WHITE);
  display.fillRoundRect(x + 25, y + 38, 28, 5, 2, TFT_BLUE);
}

void drawGameIcon(TFT_eSPI& display, uint8_t index, int16_t x, int16_t y) {
  switch (index) {
    case 1:
      drawSuperFlashyBirdsIcon(display, x + 8, y);
      break;
    case 2:  // its picture is 72 x 40: centered in the tile
      drawGalleryIcon(display, x + 10, y + 7);
      break;
    case 3:
      drawTicTacToeIcon(display, x, y);
      break;
    case 4:
      drawRockPaperScissorsIcon(display, x, y);
      break;
    case 5:
      drawRunnerThumbnail(display, x + 8, y);
      break;
    case 6:
      drawBrickBreakerIcon(display, x, y);
      break;
    case 7:
      drawGuitarIcon(display, x, y);
      break;
    case 8:
      drawPongIcon(display, x, y);
      break;
    default:
      break;
  }
}

void drawScaledFlappySprite(TFT_eSPI& display, int16_t x, int16_t y,
                            int16_t size) {
  for (int16_t destY = 0; destY < size; ++destY) {
    const int16_t sourceY =
        destY * BIRD_FLAP_CLOSED_HEIGHT / size;
    for (int16_t destX = 0; destX < size; ++destX) {
      const int16_t sourceX =
          destX * BIRD_FLAP_CLOSED_WIDTH / size;
      const uint16_t color =
          flappyBirdPixels()[sourceY * BIRD_FLAP_CLOSED_WIDTH + sourceX];
      if (color != SPRITE_TRANSPARENT) {
        display.drawPixel(x + destX, y + destY, color);
      }
    }
  }
}

// The Flappy Falcon thumbnail: its sky picture with the bird, in the same box
// as the Flashy Birds one (80 x 58, above the name).
void drawFlappyIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  constexpr int16_t ICON_W = 80;
  constexpr int16_t ICON_H = 58;
  constexpr int16_t BIRD_SIZE = 46;
  TFT_eSprite picture(&display);
  picture.setColorDepth(8);
  if (!picture.createSprite(ICON_W, ICON_H)) {
    drawScaledFlappySprite(display, x + (ICON_W - BIRD_SIZE) / 2,
                           y + (ICON_H - BIRD_SIZE) / 2, BIRD_SIZE);
    return;
  }

  // Zoom in on the building: this part of the 320 x 240 background.
  constexpr int16_t CROP_X = 62;
  constexpr int16_t CROP_Y = 64;
  constexpr int16_t CROP_W = 160;
  constexpr int16_t CROP_H = 116;
  uint8_t* const pixels = static_cast<uint8_t*>(picture.getPointer());
  for (int16_t destY = 0; destY < ICON_H; ++destY) {
    const int16_t sourceY = CROP_Y + destY * CROP_H / ICON_H;
    for (int16_t destX = 0; destX < ICON_W; ++destX) {
      const int16_t sourceX = CROP_X + destX * CROP_W / ICON_W;
      pixels[destY * ICON_W + destX] =
          flappyBackgroundPixels()[sourceY * BACKGROUND_WIDTH + sourceX];
    }
  }
  drawScaledFlappySprite(picture, (ICON_W - BIRD_SIZE) / 2,
                         (ICON_H - BIRD_SIZE) / 2, BIRD_SIZE);
  picture.pushSprite(x, y);
  picture.deleteSprite();
}

void drawMenu(TFT_eSPI& display, uint8_t selectedIndex) {
  display.fillScreen(MENU_BG);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(TC_DATUM);
  display.drawString(twoPlayer ? "2 Players" : "1 Player", display.width() / 2,
                     6);

  // Keep the selected row on screen.
  const uint8_t row = selectedIndex / 3;
  if (row < topRow) topRow = row;
  if (row >= topRow + VISIBLE_ROWS) topRow = row - VISIBLE_ROWS + 1;
  // Small arrows beside the title when there are more rows that way.
  const int16_t mid = display.width() / 2;
  if (topRow > 0) {
    display.fillTriangle(mid - 62, 18, mid - 50, 18, mid - 56, 8, TFT_YELLOW);
  }
  if (topRow + VISIBLE_ROWS < (listedCount() + 2) / 3) {
    display.fillTriangle(mid + 50, 8, mid + 62, 8, mid + 56, 18, TFT_YELLOW);
  }

  for (uint8_t slot = topRow * 3;
       slot < listedCount() && slot < (topRow + VISIBLE_ROWS) * 3; ++slot) {
    const uint8_t index = gameAt(slot);
    const int16_t x = tileX(slot);
    const int16_t y = tileY(slot);
    display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          MENU_TILE);
    display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                          slot == selectedIndex ? TFT_YELLOW : TFT_WHITE);
    if (slot == selectedIndex) {
      display.drawRoundRect(x + 2, y + 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                            TILE_RADIUS - 2, TFT_YELLOW);
    }
    if (index == 0) {
      drawFlappyIcon(display, x + 8, y + 8);
    } else {
      drawGameIcon(display, index, x, y + 8);
    }

    display.setTextFont(1);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, MENU_TILE);
    display.setTextDatum(BC_DATUM);
    if (index == 4) {  // too long for one line
      display.drawString(GAME_NAMES[index], x + TILE_WIDTH / 2,
                         y + TILE_HEIGHT - 15);
      display.drawString("Scissors", x + TILE_WIDTH / 2, y + TILE_HEIGHT - 5);
    } else {
      display.drawString(GAME_NAMES[index], x + TILE_WIDTH / 2,
                         y + TILE_HEIGHT - 7);
    }
  }

  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(BC_DATUM);
  display.drawString("1/4 Move  2 Players  3 Select  Hold 3 Off",
                     display.width() / 2,
                     display.height() - 4);
}

void launchSelectedGame(TFT_eSPI& display, uint8_t selectedIndex) {
  switch (selectedIndex) {
    case 0:
      runFlappyBird(display);
      break;
    case 1:
      runSuperFlashyBirds(display);
      break;
    case 2:
      runGallery(display);
      break;
    case 3:
      runTicTacToe(display, twoPlayer);
      break;
    case 4:
      runRockPaperScissors(display, twoPlayer);
      break;
    case 5:
      runRunner(display);
      break;
    case 6:
      runBrickBreaker(display);
      break;
    case 7:
      runGuitarHero(display);
      break;
    case 8:
      runPong(display, twoPlayer);
      break;
  }
}
}  // namespace

void showGameMenu(TFT_eSPI& display) {
  uint8_t selectedIndex = 0;
  topRow = 0;
  waitForButtonRelease();
  drawMenu(display, selectedIndex);

  uint32_t pressedAt = 0;  // when BTN3 went down in the menu
  bool showingBar = false;
  while (true) {
    const Button button = readButtonEvent();
    if (button == Button::Button1) {
      selectedIndex =
          selectedIndex == 0 ? listedCount() - 1 : selectedIndex - 1;
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button4) {
      selectedIndex = (selectedIndex + 1) % listedCount();
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button2) {
      twoPlayer = !twoPlayer;
      selectedIndex = 0;
      topRow = 0;
      drawMenu(display, selectedIndex);
    } else if (button == Button::Button3) {
      pressedAt = millis();
    }

    if (pressedAt != 0) {
      const uint32_t held = millis() - pressedAt;
      if (readButton() != Button::Button3) {  // let go
        pressedAt = 0;
        if (held < SELECT_MAX_MS) {
          waitForButtonRelease();
          launchSelectedGame(display, gameAt(selectedIndex));
        }
        showingBar = false;
        drawMenu(display, selectedIndex);
      } else if (held >= SHUTDOWN_HOLD_MS) {
        display.fillScreen(TFT_BLACK);
        display.setTextSize(2);
        display.setTextColor(TFT_WHITE, TFT_BLACK);
        display.setTextDatum(MC_DATUM);
        display.drawString("Release to turn off", display.width() / 2,
                           display.height() / 2);
        // A button still held would wake the device straight back up.
        waitForButtonRelease();
        return;
      } else if (held >= SELECT_MAX_MS) {
        const int16_t top = display.height() - 18;
        if (!showingBar) {
          display.fillRect(0, top, display.width(), 18, MENU_BG);
          display.drawRect(40, top + 4, display.width() - 80, 10, TFT_WHITE);
          showingBar = true;
        }
        display.fillRect(42, top + 6,
                         (display.width() - 84) * held / SHUTDOWN_HOLD_MS, 6,
                         TFT_YELLOW);
      }
    }
    delay(10);
  }
}

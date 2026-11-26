#include "games/game_menu.h"

#include <Arduino.h>

#include "display/battery_icon.h"
#include "game_assets.h"
#include "games/arcade_games.h"
#include "games/flappy_bird.h"
#include "games/gallery.h"
#include "games/guitar_hero.h"
#include "input/button_input.h"
#include "power/battery_monitor.h"
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
// Likewise BTN4 moves, or held, goes back where the menu was opened from.
constexpr uint32_t SELECT_MAX_MS = 600;
constexpr uint32_t SHUTDOWN_HOLD_MS = 3000;
constexpr uint32_t BACK_HOLD_MS = 1500;

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

void drawGameIcon(TFT_eSPI& display, uint8_t index, const FlappyAssets& assets,
                  int16_t x, int16_t y) {
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
      drawRunnerThumbnail(display, assets.birdOpen.pixels(), x + 8, y);
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

void drawScaledFlappySprite(TFT_eSPI& display, const FlappyAssets& assets,
                            int16_t x, int16_t y, int16_t size) {
  for (int16_t destY = 0; destY < size; ++destY) {
    const int16_t sourceY =
        destY * assets.birdBack.height / size;
    for (int16_t destX = 0; destX < size; ++destX) {
      const int16_t sourceX =
          destX * assets.birdBack.width / size;
      const uint16_t color =
          assets.birdBack.pixels()[sourceY * assets.birdBack.width + sourceX];
      if (color != 0xF81F) {
        display.drawPixel(x + destX, y + destY, color);
      }
    }
  }
}

// The Flappy Falcon thumbnail: its sky picture with the bird, in the same box
// as the Flashy Birds one (80 x 58, above the name).
void drawFlappyIcon(TFT_eSPI& display, const FlappyAssets& assets, int16_t x,
                    int16_t y) {
  constexpr int16_t ICON_W = 80;
  constexpr int16_t ICON_H = 58;
  constexpr int16_t BIRD_SIZE = 46;
  TFT_eSprite picture(&display);
  picture.setColorDepth(8);
  if (!picture.createSprite(ICON_W, ICON_H)) {
    drawScaledFlappySprite(display, assets, x + (ICON_W - BIRD_SIZE) / 2,
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
          assets.background.data[sourceY * assets.background.width + sourceX];
    }
  }
  drawScaledFlappySprite(picture, assets, (ICON_W - BIRD_SIZE) / 2,
                         (ICON_H - BIRD_SIZE) / 2, BIRD_SIZE);
  picture.pushSprite(x, y);
  picture.deleteSprite();
}

void drawTile(TFT_eSPI& display, uint8_t slot, bool selected,
              const FlappyAssets& assets) {
  const uint8_t index = gameAt(slot);
  const int16_t x = tileX(slot);
  const int16_t y = tileY(slot);
  display.fillRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS, MENU_TILE);
  display.drawRoundRect(x, y, TILE_WIDTH, TILE_HEIGHT, TILE_RADIUS,
                        selected ? TFT_YELLOW : TFT_WHITE);
  if (selected) {
    display.drawRoundRect(x + 2, y + 2, TILE_WIDTH - 4, TILE_HEIGHT - 4,
                          TILE_RADIUS - 2, TFT_YELLOW);
  }
  if (index == 0) {
    drawFlappyIcon(display, assets, x + 8, y + 8);
  } else {
    drawGameIcon(display, index, assets, x, y + 8);
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

void drawFooter(TFT_eSPI& display, bool canGoBack) {
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(BC_DATUM);
  display.drawString(canGoBack ? "1/4 Move 2 Mode 3 OK  Hold 3 Off 4 Back"
                               : "1/4 Move  2 Players  3 Select  Hold 3 Off",
                     display.width() / 2, display.height() - 4);
}

// Scroll so the selection is on screen; true if that moved the rows.
bool scrollTo(uint8_t selectedIndex) {
  const uint8_t before = topRow;
  const uint8_t row = selectedIndex / 3;
  if (row < topRow) topRow = row;
  if (row >= topRow + VISIBLE_ROWS) topRow = row - VISIBLE_ROWS + 1;
  return topRow != before;
}

void drawMenu(TFT_eSPI& display, uint8_t selectedIndex,
              const FlappyAssets& assets, bool canGoBack) {
  scrollTo(selectedIndex);
  display.fillScreen(MENU_BG);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, MENU_BG);
  display.setTextDatum(TC_DATUM);
  display.drawString(twoPlayer ? "2 Players" : "1 Player", display.width() / 2,
                     6);

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
    drawTile(display, slot, slot == selectedIndex, assets);
  }
  drawBatteryIndicator(display, batteryPercent(), MENU_BG);
  drawFooter(display, canGoBack);
}

// Get the Flappy pictures back after a game has used the memory.
void loadMenuAssets(TFT_eSPI& display, FlappyAssets& assets) {
  while (!loadFlappyAssets(assets)) {
    assets.release();
    ensureGameAssets(display);
  }
}

// The progress bar shown while a button is held.
void drawHoldBar(TFT_eSPI& display, bool& showing, uint32_t held,
                 uint32_t needed) {
  const int16_t top = display.height() - 18;
  if (!showing) {
    display.fillRect(0, top, display.width(), 18, MENU_BG);
    display.drawRect(40, top + 4, display.width() - 80, 10, TFT_WHITE);
    showing = true;
  }
  display.fillRect(42, top + 6,
                   (display.width() - 84) * min(held, needed) / needed, 6,
                   TFT_YELLOW);
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

bool showGameMenu(TFT_eSPI& display, bool canGoBack) {
  uint8_t selectedIndex = 0;
  topRow = 0;
  waitForButtonRelease();
  ensureGameAssets(display);

  FlappyAssets assets;
  SuperFlashyAssets menuArt;
  if (!takePreloadedGameAssets(assets, menuArt)) {
    loadMenuAssets(display, assets);
  }
  menuArt.release();
  loadSuperFlashyMenuArt(menuArt);
  bindSuperFlashyArt(menuArt);
  drawMenu(display, selectedIndex, assets, canGoBack);

  Button holding = Button::None;  // BTN3, or BTN4 when it can go back
  uint32_t pressedAt = 0;
  bool showingBar = false;
  int8_t shownBattery = batteryPercent();

  // Move the highlight, redrawing everything only when the rows scroll.
  const auto moveTo = [&](uint8_t next) {
    const uint8_t previous = selectedIndex;
    selectedIndex = next;
    if (scrollTo(selectedIndex)) {
      drawMenu(display, selectedIndex, assets, canGoBack);
    } else if (previous != selectedIndex) {
      drawTile(display, previous, false, assets);
      drawTile(display, selectedIndex, true, assets);
    }
  };

  while (true) {
    const Button button = readButtonEvent();
    if (button == Button::Button1) {
      moveTo(selectedIndex == 0 ? listedCount() - 1 : selectedIndex - 1);
    } else if (button == Button::Button2) {
      twoPlayer = !twoPlayer;
      selectedIndex = 0;
      topRow = 0;
      drawMenu(display, selectedIndex, assets, canGoBack);
    } else if (button == Button::Button4 && !canGoBack) {
      moveTo((selectedIndex + 1) % listedCount());
    } else if (button == Button::Button3 || button == Button::Button4) {
      holding = button;
      pressedAt = millis();
    }

    if (holding != Button::None) {
      const uint32_t held = millis() - pressedAt;
      const uint32_t needed =
          holding == Button::Button3 ? SHUTDOWN_HOLD_MS : BACK_HOLD_MS;
      if (readButton() != holding) {  // let go
        const bool tap = held < SELECT_MAX_MS;
        if (showingBar) drawMenu(display, selectedIndex, assets, canGoBack);
        showingBar = false;
        if (tap && holding == Button::Button4) {
          moveTo((selectedIndex + 1) % listedCount());
        } else if (tap) {
          waitForButtonRelease();
          assets.release();
          menuArt.release();
          launchSelectedGame(display, gameAt(selectedIndex));
          loadMenuAssets(display, assets);
          loadSuperFlashyMenuArt(menuArt);
          bindSuperFlashyArt(menuArt);
          drawMenu(display, selectedIndex, assets, canGoBack);
        }
        holding = Button::None;
      } else if (held >= needed) {
        const bool off = holding == Button::Button3;
        if (off) {
          display.fillScreen(TFT_BLACK);
          display.setTextSize(2);
          display.setTextColor(TFT_WHITE, TFT_BLACK);
          display.setTextDatum(MC_DATUM);
          display.drawString("Release to turn off", display.width() / 2,
                             display.height() / 2);
        }
        // A button still held would wake the device straight back up.
        waitForButtonRelease();
        assets.release();
        menuArt.release();
        return off;
      } else if (held >= SELECT_MAX_MS) {
        drawHoldBar(display, showingBar, held, needed);
      }
    }
    const int8_t percent = batteryPercent();
    if (percent != shownBattery && !showingBar) {
      shownBattery = percent;
      drawBatteryIndicator(display, percent, MENU_BG);
    }
    delay(10);
  }
}

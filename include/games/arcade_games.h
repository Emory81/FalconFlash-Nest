#pragma once

#include <TFT_eSPI.h>

// Small games for the menu. Holding BTN3 for 3 seconds exits all of them but
// Rock Paper Scissors, where it is BTN4 that is held.
// twoPlayer: a second person takes the computer's place, on the same buttons.
void runTicTacToe(TFT_eSPI& display, bool twoPlayer);
void runRockPaperScissors(TFT_eSPI& display, bool twoPlayer);
void runBrickBreaker(TFT_eSPI& display);
void runRunner(TFT_eSPI& display);
void runPong(TFT_eSPI& display, bool twoPlayer);

// Menu tile thumbnails, drawn from the tile's left edge.
void drawTicTacToeIcon(TFT_eSPI& display, int16_t x, int16_t y);
void drawRockPaperScissorsIcon(TFT_eSPI& display, int16_t x, int16_t y);
void drawPongIcon(TFT_eSPI& display, int16_t x, int16_t y);
// An 80 x 58 picture at (x, y); falcon is the 48 x 48 Flappy bird art.
void drawRunnerThumbnail(TFT_eSPI& display, const uint16_t* falcon, int16_t x,
                         int16_t y);

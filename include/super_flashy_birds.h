#pragma once

#include <TFT_eSPI.h>

#include "game_assets.h"

void runSuperFlashyBirds(TFT_eSPI& display);

// Small preview (about 76 x 56) for the game menu tile, drawn at (x, y).
void drawSuperFlashyBirdsIcon(TFT_eSPI& display,
                              const SuperFlashyAssets& assets, int16_t x,
                              int16_t y);

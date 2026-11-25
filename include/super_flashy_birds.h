#pragma once

#include <TFT_eSPI.h>

void runSuperFlashyBirds(TFT_eSPI& display);

// The 320 x 240 RGB332 sky, shared so other games do not store a second copy.
const uint8_t* superFlashySkyPixels();
// The whole sky shrunk into a w x h box, for menu thumbnails.
void drawSuperFlashySkyThumbnail(TFT_eSPI& display, int16_t x, int16_t y,
                                 int16_t w, int16_t h);

// Small preview (about 76 x 56) for the game menu tile, drawn at (x, y).
void drawSuperFlashyBirdsIcon(TFT_eSPI& display, int16_t x, int16_t y);

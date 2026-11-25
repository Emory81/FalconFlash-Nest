#pragma once

#include <TFT_eSPI.h>

void runSuperFlashyBirds(TFT_eSPI& display);

// The 320 x 240 sky scrolled sideways by `offset`, into an RGB332 frame
// buffer of that size. Shared so other games do not store a second copy.
void copySuperFlashySky(uint8_t* frame, int16_t offset);
// The whole sky shrunk into a w x h box, for menu thumbnails.
void drawSuperFlashySkyThumbnail(TFT_eSPI& display, int16_t x, int16_t y,
                                 int16_t w, int16_t h);

// Small preview (about 76 x 56) for the game menu tile, drawn at (x, y).
void drawSuperFlashyBirdsIcon(TFT_eSPI& display, int16_t x, int16_t y);

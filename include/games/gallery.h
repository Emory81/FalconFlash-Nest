#pragma once

#include <TFT_eSPI.h>

void runGallery(TFT_eSPI& display);

// Small preview (about 76 x 44) for the game menu tile, drawn at (x, y).
void drawGalleryIcon(TFT_eSPI& display, int16_t x, int16_t y);

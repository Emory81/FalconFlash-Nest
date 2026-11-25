#pragma once

#include <stdint.h>

#include <TFT_eSPI.h>

void runFlappyBird(TFT_eSPI& display);

// Shared sprite data for the menu tile, so game_menu.cpp doesn't carry its
// own copy of the (large) arrays from flappy_sprites.h.
const uint8_t* flappyBackgroundPixels();
const uint16_t* flappyBirdPixels();

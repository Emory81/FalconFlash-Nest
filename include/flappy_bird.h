#pragma once

#include <stdint.h>

#include <TFT_eSPI.h>

void runFlappyBird(TFT_eSPI& display);

// Shared sprite data for the menu tile, so game_menu.cpp doesn't carry its
// own copy of the (large) arrays from flappy_sprites.h.
const uint8_t* flappyBackgroundPixels();
const uint8_t* flappyNightBackgroundPixels();  // 320 x 240, RGB332
const uint16_t* flappyBirdPixels();
// 48 x 48 flap frames: 0 wings open, 1 half way, 2 closed.
const uint16_t* flappyBirdFrame(uint8_t frame);

#pragma once

#include <TFT_eSPI.h>

// Runs until the player holds BTN3 to shut off; the caller then powers down.
void showGameMenu(TFT_eSPI& display);

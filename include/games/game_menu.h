#pragma once

#include <TFT_eSPI.h>

// Runs until the player holds BTN3 to shut off (returns true) or, when
// canGoBack, holds BTN4 to leave the menu (returns false).
bool showGameMenu(TFT_eSPI& display, bool canGoBack);

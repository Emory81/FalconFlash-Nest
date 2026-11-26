#pragma once

#include <TFT_eSPI.h>

// Runs until the player holds BTN3 to shut off (returns true) or, when
// canGoBack, holds BTN4 to leave the menu (returns false). gamesOnly also
// drops the pairing and update settings once the game files are stored.
bool showGameMenu(TFT_eSPI& display, bool canGoBack, bool gamesOnly = false);

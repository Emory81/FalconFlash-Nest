#pragma once

#include <stdint.h>

constexpr uint8_t LEADERBOARD_SIZE = 5;

// Scores are kept per game, keyed by a short name such as "flappy".
// For now the leaderboard is this device's own top scores, saved in flash;
// leaderboardTop() is the single place to swap in scores from a server.

// Highest score ever saved on this device for the game (0 if none).
uint32_t leaderboardBest(const char* game);

// Records a finished game. Returns true if it is a new device best.
bool leaderboardRecordScore(const char* game, uint32_t score);

// Lifetime flap counter for the game, saved in flash and never reset.
uint32_t leaderboardTotalFlaps(const char* game);

// Adds this round's flaps to the lifetime counter and returns the new total.
uint32_t leaderboardAddFlaps(const char* game, uint32_t flaps);

// Fills `scores` (highest first) with up to maxEntries entries and returns
// how many are filled.
uint8_t leaderboardTop(const char* game, uint32_t* scores, uint8_t maxEntries);

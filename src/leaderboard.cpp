#include "leaderboard.h"

#include <Preferences.h>

namespace {
constexpr char KEY_PREFIX = 's';
constexpr const char* FLAPS_KEY = "flaps";

// Loads the saved top scores, highest first; unused slots are 0.
void loadScores(Preferences& preferences, uint32_t* scores) {
  for (uint8_t i = 0; i < LEADERBOARD_SIZE; ++i) {
    char key[4] = {KEY_PREFIX, static_cast<char>('0' + i), '\0', '\0'};
    scores[i] = preferences.getUInt(key, 0);
  }
}
}  // namespace

uint32_t leaderboardBest(const char* game) {
  uint32_t scores[LEADERBOARD_SIZE];
  Preferences preferences;
  preferences.begin(game, true);
  loadScores(preferences, scores);
  preferences.end();
  return scores[0];
}

bool leaderboardRecordScore(const char* game, uint32_t score) {
  if (score == 0) {
    return false;
  }

  uint32_t scores[LEADERBOARD_SIZE];
  Preferences preferences;
  preferences.begin(game, false);
  loadScores(preferences, scores);

  const bool newBest = score > scores[0];
  // Insert below any equal or higher score, shifting lower ones down.
  uint8_t position = 0;
  while (position < LEADERBOARD_SIZE && scores[position] >= score) {
    ++position;
  }
  if (position < LEADERBOARD_SIZE) {
    for (uint8_t i = LEADERBOARD_SIZE - 1; i > position; --i) {
      scores[i] = scores[i - 1];
    }
    scores[position] = score;
    for (uint8_t i = 0; i < LEADERBOARD_SIZE; ++i) {
      char key[4] = {KEY_PREFIX, static_cast<char>('0' + i), '\0', '\0'};
      preferences.putUInt(key, scores[i]);
    }
  }
  preferences.end();
  return newBest;
}

uint8_t leaderboardTop(const char* game, uint32_t* scores, uint8_t maxEntries) {
  uint32_t saved[LEADERBOARD_SIZE];
  Preferences preferences;
  preferences.begin(game, true);
  loadScores(preferences, saved);
  preferences.end();

  uint8_t count = 0;
  for (uint8_t i = 0; i < LEADERBOARD_SIZE && count < maxEntries; ++i) {
    if (saved[i] > 0) {
      scores[count++] = saved[i];
    }
  }
  return count;
}

uint32_t leaderboardTotalFlaps(const char* game) {
  Preferences preferences;
  preferences.begin(game, true);
  const uint32_t total = preferences.getUInt(FLAPS_KEY, 0);
  preferences.end();
  return total;
}

uint32_t leaderboardAddFlaps(const char* game, uint32_t flaps) {
  Preferences preferences;
  preferences.begin(game, false);
  const uint32_t total = preferences.getUInt(FLAPS_KEY, 0) + flaps;
  if (flaps > 0) {
    preferences.putUInt(FLAPS_KEY, total);
  }
  preferences.end();
  return total;
}

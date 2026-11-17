#pragma once

#include <stdint.h>

#include <stddef.h>

// A decoded manifest entry backed by a heap allocation. RGB565 images are
// stored little-endian so the returned pixel pointer can be used directly on
// the ESP32.
struct GameAssetImage {
  uint8_t* data = nullptr;
  uint32_t size = 0;
  uint16_t width = 0;
  uint16_t height = 0;

  const uint16_t* pixels() const {
    return reinterpret_cast<const uint16_t*>(data);
  }

  void release();
};

struct FlappyAssets {
  GameAssetImage birdClosed;
  GameAssetImage birdBack;
  GameAssetImage birdTransition;
  GameAssetImage birdOpen;
  GameAssetImage pipeBody;
  GameAssetImage pipeCap;
  GameAssetImage backgroundDay;
  GameAssetImage backgroundNight;

  void release();
};

struct SuperFlashyAssets {
  GameAssetImage bird;
  GameAssetImage walk[8];
  GameAssetImage duck;
  GameAssetImage gorilla;
  GameAssetImage redGorilla;
  GameAssetImage dirt;
  GameAssetImage grass;
  GameAssetImage box;
  GameAssetImage mystery;
  GameAssetImage mysteryOff;
  GameAssetImage egg;
  GameAssetImage blueEgg;
  GameAssetImage rat;
  GameAssetImage fox;
  GameAssetImage nest;
  GameAssetImage eggCracking;
  GameAssetImage eggOpen;

  void release();
};

// Mount the cache before the display starts using game graphics. This is
// intentionally non-formatting: a mount failure must not erase a valid cache.
bool initGameAssets();

// Start a bounded synchronization task. It returns immediately and leaves
// existing cached files in place if Wi-Fi or GitHub is unavailable.
void startGameAssetSync();

bool loadFlappyAssets(FlappyAssets& assets);
bool loadSuperFlashyAssets(SuperFlashyAssets& assets);

#pragma once

#include <stdint.h>

#include <stddef.h>

#include <TFT_eSPI.h>

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
  GameAssetImage background;

  void release();
};

struct SuperFlashyAssets {
  GameAssetImage bird;
  GameAssetImage walk[8];
  GameAssetImage duck;
  GameAssetImage dive;
  GameAssetImage gorilla;
  GameAssetImage redGorilla;
  GameAssetImage dirt;
  GameAssetImage dirtShadow;
  GameAssetImage grass;
  GameAssetImage box;
  GameAssetImage mystery;
  GameAssetImage mysteryOff;
  GameAssetImage egg;
  GameAssetImage blueEgg;
  GameAssetImage greenEgg;
  GameAssetImage rat;
  GameAssetImage fox;
  GameAssetImage nest;
  GameAssetImage eggCracking;
  GameAssetImage eggOpen;
  GameAssetImage goldEgg;
  GameAssetImage infinityEgg;
  GameAssetImage bean;
  GameAssetImage arrowRight;
  GameAssetImage arrowLeft;
  GameAssetImage arrowUp;
  GameAssetImage arrowDown;
  GameAssetImage grassyArrowRight;
  GameAssetImage grassyArrowLeft;
  GameAssetImage grassyArrowUp;
  GameAssetImage grassyArrowDown;
  GameAssetImage cloud;
  GameAssetImage coin;
  GameAssetImage yellowGrass;
  GameAssetImage cloudBackground;
  GameAssetImage cardboardBox;
  GameAssetImage tennisBall;
  GameAssetImage whiteArrowRight;
  GameAssetImage whiteArrowLeft;
  GameAssetImage whiteArrowUp;
  GameAssetImage whiteArrowDown;
  GameAssetImage golfEggUp;
  GameAssetImage golfEggDown;
  GameAssetImage sky;

  void release();
};

// Mount the cache before the display starts using game graphics. This is
// intentionally non-formatting: a mount failure must not erase a valid cache.
bool initGameAssets();

// Start downloading missing game files without blocking the current screen.
// This is called once the timer screen is visible.
void startGameAssetPrefetch();

// Start decoding the cached game graphics into RAM without blocking the
// current screen. This is called once the timer screen is visible.
void startGameAssetMemoryPreload();

// Finish any timer-screen asset tasks and release decoded graphics so HTTPS
// order submission has the Wi-Fi/TLS connection and heap to itself.
void prepareGameAssetsForOrderRequest();

// Temporarily suspend background asset tasks so bit-banged DAC audio is not
// distorted by Wi-Fi downloads or decode work on the other core.
void quietGameAssetBackgroundForAudio();

// Move the decoded graphics into the game menu's ownership. Returns false if
// the background preload has not finished yet.
bool takePreloadedGameAssets(FlappyAssets& flappy, SuperFlashyAssets& super);

// Block until every manifest asset is cached and verified. A progress screen
// remains visible while Wi-Fi/GitHub is unavailable and while files download.
bool ensureGameAssets(TFT_eSPI& display);

bool loadFlappyAssets(FlappyAssets& assets, bool night = false,
                      bool withBackground = true);
// One cached file. width and height of 0 accept the manifest's size.
bool loadGameAssetImage(const char* path, const char* format,
                        GameAssetImage& image);
// Swap only the background of loaded assets, reusing its memory.
bool switchFlappyBackground(FlappyAssets& assets, bool night);
bool loadSuperFlashyAssets(SuperFlashyAssets& assets, bool withSky = true);
// Sky plus the few sprites the game-menu tiles draw.
bool loadSuperFlashyMenuArt(SuperFlashyAssets& assets);
// Point the game sprite names at a loaded set. Empty images become null.
void bindSuperFlashyArt(const SuperFlashyAssets& assets);
bool loadDeskSpotifyLogo(GameAssetImage& logo);

// Let other modules (e.g. the battery log) share the LittleFS cache safely.
// Returns false if the lock could not be taken within the timeout.
bool lockGameAssetsFilesystem(uint32_t timeoutMs);
void unlockGameAssetsFilesystem();

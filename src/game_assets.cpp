#include "game_assets.h"

#include <Arduino.h>
#include <FS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#include <cstdlib>
#include <cstring>
#include <memory>

#include "device_config.h"
#include "network/device_network.h"
#include "super_flashy_art.h"

namespace {

constexpr char CACHE_ROOT[] = "/game_assets";
constexpr char MANIFEST_PATH[] = "/game_assets/manifest.txt";
constexpr char MANIFEST_PART_PATH[] = "/game_assets/manifest.txt.part";
constexpr char MANIFEST_BACKUP_PATH[] = "/game_assets/manifest.txt.bak";
constexpr uint8_t MAX_MANIFEST_ENTRIES = 72;
constexpr uint16_t HASH_HEX_LENGTH = 64;
constexpr uint32_t HTTP_TIMEOUT_MS = 12000;
constexpr uint32_t RETRY_DELAY_MS = 1000;

struct ManifestEntry {
  char path[64];
  char format[8];
  char hash[HASH_HEX_LENGTH + 1];
  uint16_t width;
  uint16_t height;
  uint32_t size;
};

// One manifest is about 11 KB. It is borrowed from the heap for each use: a
// permanent copy per function left too little room for the games.
using ManifestEntries = std::unique_ptr<ManifestEntry[], void (*)(void*)>;

ManifestEntries makeEntries() {
  return ManifestEntries(static_cast<ManifestEntry*>(calloc(
                             MAX_MANIFEST_ENTRIES, sizeof(ManifestEntry))),
                         &free);
}

bool filesystemReady = false;
SemaphoreHandle_t filesystemMutex = nullptr;
bool progressScreenReady = false;
String progressStatus;
TaskHandle_t assetSyncTaskHandle = nullptr;
volatile bool assetSyncTaskRunning = false;
volatile bool assetSyncTaskSucceeded = false;
TaskHandle_t assetMemoryPreloadTaskHandle = nullptr;
volatile bool assetMemoryPreloadRunning = false;
volatile bool assetMemoryPreloadReady = false;
FlappyAssets preloadedFlappyAssets;
SuperFlashyAssets preloadedSuperFlashyAssets;

void drawAssetProgress(TFT_eSPI& display, const char* status,
                       uint32_t completed, uint32_t total) {
  const int16_t screenWidth = display.width();
  const int16_t centerX = screenWidth / 2;
  const int16_t barX = 30;
  const int16_t barY = 142;
  const int16_t barWidth = screenWidth - 60;
  const int16_t barHeight = 18;
  const uint8_t percent =
      total == 0 ? 0 : static_cast<uint8_t>(
                             min<uint32_t>(100, completed * 100UL / total));

  if (!progressScreenReady) {
    display.fillScreen(TFT_BLACK);
    display.setTextFont(1);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.setTextSize(2);
    display.drawString("Loading game files", centerX, 74);
    display.drawRoundRect(barX, barY, barWidth, barHeight, 5, TFT_WHITE);
    progressScreenReady = true;
    progressStatus = "";
  }
  display.setTextFont(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.setTextSize(1);
  if (progressStatus != status) {
    display.fillRect(0, 99, screenWidth, 23, TFT_BLACK);
    display.drawString(status, centerX, 110);
    progressStatus = status;
  }
  display.fillRoundRect(barX + 2, barY + 2, barWidth - 4, barHeight - 4, 3,
                        TFT_BLACK);
  const int16_t fillWidth =
      static_cast<int16_t>((barWidth - 4) * percent / 100);
  if (fillWidth > 0) {
    display.fillRoundRect(barX + 2, barY + 2, fillWidth, barHeight - 4, 3,
                          TFT_WHITE);
  }
  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", percent);
  display.fillRect(centerX - 25, 172, 50, 22, TFT_BLACK);
  display.drawString(percentText, centerX, 183);
}

void lockFilesystem() {
  if (filesystemMutex != nullptr) {
    xSemaphoreTake(filesystemMutex, portMAX_DELAY);
  }
}

void unlockFilesystem() {
  if (filesystemMutex != nullptr) {
    xSemaphoreGive(filesystemMutex);
  }
}

void copyString(char* destination, size_t destinationSize,
                const String& source) {
  if (destinationSize == 0) return;
  source.toCharArray(destination, destinationSize);
  destination[destinationSize - 1] = '\0';
}

bool parseManifestLine(const String& line, ManifestEntry& entry) {
  if (line.length() == 0 || line[0] == '#') return false;
  if (line.startsWith("manifest_version|")) return false;

  int separator[5];
  int start = 0;
  for (int index = 0; index < 5; ++index) {
    separator[index] = line.indexOf('|', start);
    if (separator[index] < 0) return false;
    start = separator[index] + 1;
  }

  const String path = line.substring(0, separator[0]);
  const String width = line.substring(separator[0] + 1, separator[1]);
  const String height = line.substring(separator[1] + 1, separator[2]);
  const String format = line.substring(separator[2] + 1, separator[3]);
  const String size = line.substring(separator[3] + 1, separator[4]);
  const String hash = line.substring(separator[4] + 1);
  if (path.length() == 0 || width.length() == 0 || height.length() == 0 ||
      format.length() == 0 || size.length() == 0 ||
      hash.length() != HASH_HEX_LENGTH) {
    return false;
  }

  copyString(entry.path, sizeof(entry.path), path);
  copyString(entry.format, sizeof(entry.format), format);
  copyString(entry.hash, sizeof(entry.hash), hash);
  entry.width = static_cast<uint16_t>(width.toInt());
  entry.height = static_cast<uint16_t>(height.toInt());
  entry.size = static_cast<uint32_t>(size.toInt());
  return entry.width > 0 && entry.height > 0 && entry.size > 0;
}

size_t parseManifest(const String& manifest, ManifestEntry* entries,
                     size_t capacity) {
  size_t count = 0;
  int lineStart = 0;
  while (lineStart < manifest.length() && count < capacity) {
    int lineEnd = manifest.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = manifest.length();
    String line = manifest.substring(lineStart, lineEnd);
    line.trim();
    ManifestEntry entry{};
    if (parseManifestLine(line, entry)) {
      entries[count++] = entry;
    }
    lineStart = lineEnd + 1;
  }
  return count;
}

bool readCachedManifest(String& manifest) {
  if (!filesystemReady) return false;
  lockFilesystem();
  File file = LittleFS.open(MANIFEST_PATH, FILE_READ);
  if (!file) {
    unlockFilesystem();
    return false;
  }
  manifest = file.readString();
  file.close();
  unlockFilesystem();
  return manifest.length() > 0;
}

bool findManifestEntry(const ManifestEntry* entries, size_t count,
                       const char* path, ManifestEntry& result) {
  for (size_t index = 0; index < count; ++index) {
    if (strcmp(entries[index].path, path) == 0) {
      result = entries[index];
      return true;
    }
  }
  return false;
}

String cachePath(const ManifestEntry& entry) {
  return String(CACHE_ROOT) + "/" + entry.path;
}

String remoteUrl(const char* path) {
  String base = deviceConfig().gameAssetBaseUrl;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + "/" + path;
}

bool validRelativePath(const char* path) {
  return path[0] != '/' && strstr(path, "..") == nullptr &&
         strstr(path, "//") == nullptr;
}

void ensureAssetDirectories() {
  lockFilesystem();
  if (!LittleFS.exists(CACHE_ROOT)) LittleFS.mkdir(CACHE_ROOT);
  if (!LittleFS.exists("/game_assets/flappy")) {
    LittleFS.mkdir("/game_assets/flappy");
  }
  if (!LittleFS.exists("/game_assets/super")) {
    LittleFS.mkdir("/game_assets/super");
  }
  if (!LittleFS.exists("/game_assets/desk")) {
    LittleFS.mkdir("/game_assets/desk");
  }
  unlockFilesystem();
}

void removeTemporaryFiles() {
  lockFilesystem();
  LittleFS.remove(MANIFEST_PART_PATH);
  LittleFS.remove(MANIFEST_BACKUP_PATH);
  unlockFilesystem();
}

bool sha256Bytes(const uint8_t* data, size_t size, char* output,
                 size_t outputSize) {
  if (outputSize < HASH_HEX_LENGTH + 1) return false;
  uint8_t digest[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  const int startStatus = mbedtls_sha256_starts_ret(&context, 0);
  const int updateStatus =
      startStatus == 0 ? mbedtls_sha256_update_ret(&context, data, size) : -1;
  const int finishStatus =
      updateStatus == 0 ? mbedtls_sha256_finish_ret(&context, digest) : -1;
  mbedtls_sha256_free(&context);
  if (finishStatus != 0) return false;

  for (uint8_t index = 0; index < sizeof(digest); ++index) {
    snprintf(output + index * 2, outputSize - index * 2, "%02x",
             digest[index]);
  }
  output[HASH_HEX_LENGTH] = '\0';
  return true;
}

bool hashesEqual(const char* left, const char* right) {
  for (uint8_t index = 0; index < HASH_HEX_LENGTH; ++index) {
    char a = left[index];
    char b = right[index];
    if (a >= 'A' && a <= 'F') a = static_cast<char>(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'F') b = static_cast<char>(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

bool fileMatchesPath(const String& path, const ManifestEntry& entry) {
  lockFilesystem();
  File file = LittleFS.open(path, FILE_READ);
  if (!file || file.size() != entry.size) {
    if (file) file.close();
    unlockFilesystem();
    return false;
  }

  mbedtls_sha256_context context;
  uint8_t buffer[1024];
  uint8_t digest[32];
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
  uint32_t total = 0;
  while (ok && total < entry.size) {
    const size_t requested =
        min<size_t>(sizeof(buffer), entry.size - total);
    const size_t read = file.read(buffer, requested);
    if (read == 0) {
      ok = false;
      break;
    }
    if (mbedtls_sha256_update_ret(&context, buffer, read) != 0) {
      ok = false;
      break;
    }
    total += read;
    // A full-cache check can run for seconds. Let the idle task run so the
    // watchdog does not reset the device while the screen says Finalizing.
    if ((total & 0xfff) < read) delay(1);
  }
  if (ok && mbedtls_sha256_finish_ret(&context, digest) != 0) ok = false;
  file.close();
  mbedtls_sha256_free(&context);
  unlockFilesystem();

  if (!ok || total != entry.size) return false;
  char actual[HASH_HEX_LENGTH + 1];
  for (uint8_t index = 0; index < sizeof(digest); ++index) {
    snprintf(actual + index * 2, sizeof(actual) - index * 2, "%02x",
             digest[index]);
  }
  actual[HASH_HEX_LENGTH] = '\0';
  return hashesEqual(actual, entry.hash);
}

bool fileMatches(const ManifestEntry& entry) {
  return fileMatchesPath(cachePath(entry), entry);
}

bool replaceCacheFile(const String& temporary, const String& target,
                      const String& backup) {
  lockFilesystem();
  LittleFS.remove(backup);
  const bool hadOldFile = LittleFS.exists(target);
  if (hadOldFile && !LittleFS.rename(target, backup)) {
    unlockFilesystem();
    return false;
  }
  if (!LittleFS.rename(temporary, target)) {
    if (hadOldFile) LittleFS.rename(backup, target);
    unlockFilesystem();
    return false;
  }
  LittleFS.remove(backup);
  unlockFilesystem();
  return true;
}

bool fetchManifest(String& manifest) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, remoteUrl("manifest.txt"))) return false;
  http.setTimeout(HTTP_TIMEOUT_MS);
  const int status = http.GET();
  if (status == HTTP_CODE_OK) manifest = http.getString();
  http.end();
  return status == HTTP_CODE_OK && manifest.length() > 0;
}

bool downloadAsset(const ManifestEntry& entry, TFT_eSPI* display,
                   uint32_t completedBefore, uint32_t totalSize) {
  if (!validRelativePath(entry.path)) return false;
  const String target = cachePath(entry);
  const String temporary = target + ".part";
  const String backup = target + ".bak";

  lockFilesystem();
  LittleFS.remove(temporary);
  unlockFilesystem();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, remoteUrl(entry.path))) return false;
  http.setTimeout(HTTP_TIMEOUT_MS);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength >= 0 &&
      static_cast<uint32_t>(contentLength) != entry.size) {
    http.end();
    return false;
  }

  File file;
  lockFilesystem();
  file = LittleFS.open(temporary, FILE_WRITE);
  unlockFilesystem();
  if (!file) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[1024];
  uint32_t total = 0;
  uint32_t lastDataAt = millis();
  bool ok = true;
  while (ok && total < entry.size &&
         (http.connected() || stream->available())) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataAt > HTTP_TIMEOUT_MS) {
        ok = false;
        break;
      }
      delay(2);
      continue;
    }

    const size_t requested =
        min<size_t>(sizeof(buffer), min<uint32_t>(available,
                                                   entry.size - total));
    const size_t read = stream->readBytes(buffer, requested);
    if (read == 0) {
      ok = false;
      break;
    }
    lockFilesystem();
    const size_t written = file.write(buffer, read);
    unlockFilesystem();
    if (written != read) {
      ok = false;
      break;
    }
    total += read;
    lastDataAt = millis();
    if (display != nullptr) {
      drawAssetProgress(*display, entry.path, completedBefore + total,
                        totalSize);
    }
  }

  lockFilesystem();
  file.close();
  unlockFilesystem();
  http.end();

  if (!ok || total != entry.size || !fileMatchesPath(temporary, entry)) {
    lockFilesystem();
    LittleFS.remove(temporary);
    unlockFilesystem();
    return false;
  }
  return replaceCacheFile(temporary, target, backup);
}

bool writeManifestAndReplace(const String& manifest) {
  lockFilesystem();
  LittleFS.remove(MANIFEST_PART_PATH);
  File file = LittleFS.open(MANIFEST_PART_PATH, FILE_WRITE);
  if (!file) {
    unlockFilesystem();
    return false;
  }
  const size_t written = file.print(manifest);
  file.close();
  unlockFilesystem();
  if (written != manifest.length()) return false;
  return replaceCacheFile(MANIFEST_PART_PATH, MANIFEST_PATH,
                          MANIFEST_BACKUP_PATH);
}

bool manifestFilesComplete(const String& manifest) {
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  const size_t count = parseManifest(manifest, entries, MAX_MANIFEST_ENTRIES);
  if (count == 0) return false;
  for (size_t index = 0; index < count; ++index) {
    if (!fileMatches(entries[index])) return false;
  }
  return true;
}

bool synchronizeAssets(TFT_eSPI* display) {
  String remoteManifest;
  if (!fetchManifest(remoteManifest)) return false;

  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  const size_t count =
      parseManifest(remoteManifest, entries, MAX_MANIFEST_ENTRIES);
  if (count == 0) return false;

  uint32_t totalSize = 0;
  uint32_t completed = 0;
  for (size_t index = 0; index < count; ++index) {
    totalSize += entries[index].size;
    if (fileMatches(entries[index])) completed += entries[index].size;
  }
  if (display != nullptr) {
    drawAssetProgress(*display, "Checking files...", completed, totalSize);
  }

  for (size_t index = 0; index < count; ++index) {
    if (fileMatches(entries[index])) continue;
    if (!downloadAsset(entries[index], display, completed, totalSize)) {
      return false;
    }
    completed += entries[index].size;
  }
  if (display != nullptr) {
    drawAssetProgress(*display, "Finalizing...", totalSize, totalSize);
  }
  return writeManifestAndReplace(remoteManifest);
}

void assetSyncTask(void*) {
  bool succeeded = false;
  if (initGameAssets()) {
    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - startedAt < HTTP_TIMEOUT_MS) {
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      succeeded = synchronizeAssets(nullptr);
    }
  }
  assetSyncTaskSucceeded = succeeded;
  assetSyncTaskRunning = false;
  assetSyncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void assetMemoryPreloadTask(void*) {
  // Let the downloader finish before reading the manifest and its files.
  while (assetSyncTaskRunning) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }

  FlappyAssets flappyAssets;
  const bool succeeded = initGameAssets() && loadFlappyAssets(flappyAssets);
  if (succeeded) {
    lockFilesystem();
    preloadedFlappyAssets.release();
    preloadedFlappyAssets = flappyAssets;
    flappyAssets = FlappyAssets{};
    assetMemoryPreloadReady = true;
    unlockFilesystem();
  } else {
    flappyAssets.release();
  }

  assetMemoryPreloadRunning = false;
  assetMemoryPreloadTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

bool loadImage(const ManifestEntry* entries, size_t count, const char* path,
               const char* format, uint16_t width, uint16_t height,
               GameAssetImage& image) {
  ManifestEntry entry{};
  if (!findManifestEntry(entries, count, path, entry) ||
      strcmp(entry.format, format) != 0 ||
      (width != 0 && entry.width != width) ||
      (height != 0 && entry.height != height)) {
    return false;
  }

  // An image that already holds a buffer of this size is read into it, so no
  // new large block of memory has to be found.
  const bool reuse = image.data != nullptr && image.size == entry.size;
  uint8_t* data =
      reuse ? image.data : static_cast<uint8_t*>(malloc(entry.size));
  if (data == nullptr) return false;

  const String cachedPath = cachePath(entry);
  lockFilesystem();
  File file = LittleFS.open(cachedPath, FILE_READ);
  bool ok = file && file.size() == entry.size;
  size_t total = 0;
  while (ok && total < entry.size) {
    const size_t read = file.read(data + total, entry.size - total);
    if (read == 0) {
      ok = false;
      break;
    }
    total += read;
    if ((total & 0xfff) < read) delay(1);
  }
  if (file) file.close();
  unlockFilesystem();

  char actual[HASH_HEX_LENGTH + 1];
  ok = ok && total == entry.size &&
       sha256Bytes(data, entry.size, actual, sizeof(actual)) &&
       hashesEqual(actual, entry.hash);
  if (!ok) {
    free(data);
    if (reuse) image = GameAssetImage{};
    return false;
  }

  image.data = data;
  image.size = entry.size;
  image.width = entry.width;
  image.height = entry.height;
  return true;
}

bool loadManifestEntries(ManifestEntry* entries, size_t& count) {
  String manifest;
  if (!readCachedManifest(manifest)) return false;
  count = parseManifest(manifest, entries, MAX_MANIFEST_ENTRIES);
  return count > 0;
}

bool loadOneImage(const char* path, const char* format, uint16_t width,
                  uint16_t height, GameAssetImage& image,
                  const ManifestEntry* entries, size_t count) {
  return loadImage(entries, count, path, format, width, height, image);
}

}  // namespace

void GameAssetImage::release() {
  if (data != nullptr) free(data);
  data = nullptr;
  size = 0;
  width = 0;
  height = 0;
}

void FlappyAssets::release() {
  birdClosed.release();
  birdBack.release();
  birdTransition.release();
  birdOpen.release();
  pipeBody.release();
  pipeCap.release();
  background.release();
}

void SuperFlashyAssets::release() {
  bird.release();
  for (auto& frame : walk) frame.release();
  duck.release();
  dive.release();
  gorilla.release();
  redGorilla.release();
  dirt.release();
  dirtShadow.release();
  grass.release();
  box.release();
  mystery.release();
  mysteryOff.release();
  egg.release();
  blueEgg.release();
  greenEgg.release();
  rat.release();
  fox.release();
  nest.release();
  eggCracking.release();
  eggOpen.release();
  goldEgg.release();
  infinityEgg.release();
  bean.release();
  arrowRight.release();
  arrowLeft.release();
  arrowUp.release();
  arrowDown.release();
  grassyArrowRight.release();
  grassyArrowLeft.release();
  grassyArrowUp.release();
  grassyArrowDown.release();
  cloud.release();
  coin.release();
  yellowGrass.release();
  cloudBackground.release();
  cardboardBox.release();
  tennisBall.release();
  whiteArrowRight.release();
  whiteArrowLeft.release();
  whiteArrowUp.release();
  whiteArrowDown.release();
  golfEggUp.release();
  golfEggDown.release();
  sky.release();
}

bool initGameAssets() {
  if (filesystemReady) return true;
  if (!LittleFS.begin(false)) return false;
  filesystemMutex = xSemaphoreCreateMutex();
  if (filesystemMutex == nullptr) {
    LittleFS.end();
    return false;
  }
  filesystemReady = true;
  ensureAssetDirectories();
  removeTemporaryFiles();
  return true;
}

bool lockGameAssetsFilesystem(uint32_t timeoutMs) {
  if (filesystemMutex == nullptr) return true;
  return xSemaphoreTake(filesystemMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockGameAssetsFilesystem() { unlockFilesystem(); }

void startGameAssetPrefetch() {
  if (assetSyncTaskRunning) return;
  if (!filesystemReady && !initGameAssets()) return;

  assetSyncTaskSucceeded = false;
  assetSyncTaskRunning = true;
  const BaseType_t created = xTaskCreatePinnedToCore(
      assetSyncTask, "game_asset_sync", 8192, nullptr, 1,
      &assetSyncTaskHandle, 0);
  if (created != pdPASS) {
    assetSyncTaskHandle = nullptr;
    assetSyncTaskRunning = false;
  }
}

void startGameAssetMemoryPreload() {
  if (assetMemoryPreloadRunning || assetMemoryPreloadReady) return;
  if (!filesystemReady && !initGameAssets()) return;

  assetMemoryPreloadRunning = true;
  const BaseType_t created = xTaskCreatePinnedToCore(
      assetMemoryPreloadTask, "game_asset_ram", 8192, nullptr, 1,
      &assetMemoryPreloadTaskHandle, 0);
  if (created != pdPASS) {
    assetMemoryPreloadTaskHandle = nullptr;
    assetMemoryPreloadRunning = false;
  }
}

void prepareGameAssetsForOrderRequest() {
  while (assetSyncTaskRunning || assetMemoryPreloadRunning) {
    delay(20);
  }

  lockFilesystem();
  preloadedFlappyAssets.release();
  preloadedSuperFlashyAssets.release();
  assetMemoryPreloadReady = false;
  unlockFilesystem();
}

void quietGameAssetBackgroundForAudio() {
  if (assetSyncTaskHandle != nullptr) {
    vTaskSuspend(assetSyncTaskHandle);
  }
  if (assetMemoryPreloadTaskHandle != nullptr) {
    vTaskSuspend(assetMemoryPreloadTaskHandle);
  }
  // Let any in-flight flash/WiFi work drain so DAC bit-banging stays steady.
  delay(30);
}

bool takePreloadedGameAssets(FlappyAssets& flappy, SuperFlashyAssets& super) {
  while (assetMemoryPreloadRunning) {
    delay(10);
  }

  lockFilesystem();
  if (!assetMemoryPreloadReady) {
    unlockFilesystem();
    return false;
  }

  flappy.release();
  super.release();
  flappy = preloadedFlappyAssets;
  super = preloadedSuperFlashyAssets;
  preloadedFlappyAssets = FlappyAssets{};
  preloadedSuperFlashyAssets = SuperFlashyAssets{};
  assetMemoryPreloadReady = false;
  unlockFilesystem();
  return true;
}

bool ensureGameAssets(TFT_eSPI& display) {
  progressScreenReady = false;
  progressStatus = "";
  drawAssetProgress(display, "Connecting to game files...", 0, 0);
  while (true) {
    if (!filesystemReady && !initGameAssets()) {
      drawAssetProgress(display, "Storage unavailable - retrying", 0, 0);
      delay(RETRY_DELAY_MS);
      continue;
    }

    if (assetSyncTaskRunning) {
      drawAssetProgress(display, "Finishing game files...", 0, 0);
      delay(100);
      continue;
    }

    String cachedManifest;
    const bool cachedComplete =
        readCachedManifest(cachedManifest) &&
        manifestFilesComplete(cachedManifest);
    if (WiFi.status() != WL_CONNECTED) {
      if (cachedComplete) {
        progressScreenReady = false;
        return true;
      }
      startDeviceWiFi();
      drawAssetProgress(display, "Waiting for Wi-Fi...", 0, 0);
      delay(250);
      continue;
    }

    if (synchronizeAssets(&display)) {
      progressScreenReady = false;
      return true;
    }
    if (cachedComplete) {
      progressScreenReady = false;
      return true;
    }
    drawAssetProgress(display, "Download failed - retrying", 0, 0);
    delay(RETRY_DELAY_MS);
  }
}

bool loadGameAssetImage(const char* path, const char* format,
                        GameAssetImage& image) {
  if (!initGameAssets()) return false;
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  return loadOneImage(path, format, 0, 0, image, entries, count);
}

bool loadFlappyAssets(FlappyAssets& assets, bool night, bool withBackground) {
  assets.release();
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;

  // The background is the one large block. Take it before the smaller
  // sprites so those cannot split the only free gap that fits it.
  bool ok = true;
  if (withBackground) {
    ok = loadOneImage(night ? "flappy/background_night.rgb332"
                            : "flappy/background_day.rgb332",
                      "rgb332", 320, 240, assets.background, entries, count);
  }
  ok = ok &&
       loadOneImage("flappy/bird_closed.rgb565", "rgb565", 48, 48,
                    assets.birdClosed, entries, count) &&
       loadOneImage("flappy/bird_back.rgb565", "rgb565", 48, 48,
                    assets.birdBack, entries, count) &&
       loadOneImage("flappy/bird_transition.rgb565", "rgb565", 48, 48,
                    assets.birdTransition, entries, count) &&
       loadOneImage("flappy/bird_open.rgb565", "rgb565", 48, 48,
                    assets.birdOpen, entries, count) &&
       loadOneImage("flappy/pipe_body.rgb565", "rgb565", 28, 8,
                    assets.pipeBody, entries, count) &&
       loadOneImage("flappy/pipe_cap.rgb565", "rgb565", 32, 14,
                    assets.pipeCap, entries, count);
  if (!ok) assets.release();
  return ok;
}

bool loadSuperFlashyAssets(SuperFlashyAssets& assets, bool withSky) {
  assets.release();
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;

  const auto load = [&](const char* path, const char* format,
                        GameAssetImage& image) {
    return loadOneImage(path, format, 0, 0, image, entries, count);
  };
  bool ok =
      load("super/bird.rgb565", "rgb565", assets.bird) &&
      load("super/walk0.rgb565", "rgb565", assets.walk[0]) &&
      load("super/walk1.rgb565", "rgb565", assets.walk[1]) &&
      load("super/walk2.rgb565", "rgb565", assets.walk[2]) &&
      load("super/walk3.rgb565", "rgb565", assets.walk[3]) &&
      load("super/walk4.rgb565", "rgb565", assets.walk[4]) &&
      load("super/walk5.rgb565", "rgb565", assets.walk[5]) &&
      load("super/walk6.rgb565", "rgb565", assets.walk[6]) &&
      load("super/walk7.rgb565", "rgb565", assets.walk[7]) &&
      load("super/duck.rgb565", "rgb565", assets.duck) &&
      load("super/dive.rgb565", "rgb565", assets.dive) &&
      load("super/gorilla.rgb565", "rgb565", assets.gorilla) &&
      load("super/red_gorilla.rgb565", "rgb565", assets.redGorilla) &&
      load("super/dirt.rgb565", "rgb565", assets.dirt) &&
      load("super/dirt_shadow.rgb565", "rgb565", assets.dirtShadow) &&
      load("super/grass.rgb565", "rgb565", assets.grass) &&
      load("super/box.rgb565", "rgb565", assets.box) &&
      load("super/mystery.rgb565", "rgb565", assets.mystery) &&
      load("super/mystery_off.rgb565", "rgb565", assets.mysteryOff) &&
      load("super/egg.rgb565", "rgb565", assets.egg) &&
      load("super/blue_egg.rgb565", "rgb565", assets.blueEgg) &&
      load("super/green_egg.rgb565", "rgb565", assets.greenEgg) &&
      load("super/rat.rgb565", "rgb565", assets.rat) &&
      load("super/fox.rgb565", "rgb565", assets.fox) &&
      load("super/nest.rgb565", "rgb565", assets.nest) &&
      load("super/egg_cracking.rgb565", "rgb565", assets.eggCracking) &&
      load("super/egg_open.rgb565", "rgb565", assets.eggOpen) &&
      load("super/gold_egg.rgb565", "rgb565", assets.goldEgg) &&
      load("super/infinity_egg.rgb565", "rgb565", assets.infinityEgg) &&
      load("super/bean.rgb565", "rgb565", assets.bean) &&
      load("super/arrow_right.rgb565", "rgb565", assets.arrowRight) &&
      load("super/arrow_left.rgb565", "rgb565", assets.arrowLeft) &&
      load("super/arrow_up.rgb565", "rgb565", assets.arrowUp) &&
      load("super/arrow_down.rgb565", "rgb565", assets.arrowDown) &&
      load("super/grassy_arrow_right.rgb565", "rgb565", assets.grassyArrowRight) &&
      load("super/grassy_arrow_left.rgb565", "rgb565", assets.grassyArrowLeft) &&
      load("super/grassy_arrow_up.rgb565", "rgb565", assets.grassyArrowUp) &&
      load("super/grassy_arrow_down.rgb565", "rgb565", assets.grassyArrowDown) &&
      load("super/cloud.rgb565", "rgb565", assets.cloud) &&
      load("super/coin.rgb565", "rgb565", assets.coin) &&
      load("super/yellow_grass.rgb565", "rgb565", assets.yellowGrass) &&
      load("super/cloud_background.rgb565", "rgb565", assets.cloudBackground) &&
      load("super/cardboard_box.rgb565", "rgb565", assets.cardboardBox) &&
      load("super/tennis_ball.rgb565", "rgb565", assets.tennisBall) &&
      load("super/white_arrow_right.rgb565", "rgb565", assets.whiteArrowRight) &&
      load("super/white_arrow_left.rgb565", "rgb565", assets.whiteArrowLeft) &&
      load("super/white_arrow_up.rgb565", "rgb565", assets.whiteArrowUp) &&
      load("super/white_arrow_down.rgb565", "rgb565", assets.whiteArrowDown) &&
      load("golf/egg_up.rgb565", "rgb565", assets.golfEggUp) &&
      load("golf/egg_down.rgb565", "rgb565", assets.golfEggDown);
  if (ok && withSky) {
    ok = load("super/sky.rle", "rle332", assets.sky);
  }
  if (!ok) assets.release();
  return ok;
}

namespace {
void setArt(const GameAssetImage& image, const uint16_t*& pixels,
            uint16_t& width, uint16_t& height) {
  pixels = image.data == nullptr ? nullptr : image.pixels();
  width = image.width;
  height = image.height;
}
}  // namespace

const uint16_t* sfbBird = nullptr;
uint16_t sfbBirdWidth = 0;
uint16_t sfbBirdHeight = 0;
const uint16_t* sfbWalk0 = nullptr;
uint16_t sfbWalk0Width = 0;
uint16_t sfbWalk0Height = 0;
const uint16_t* sfbWalk1 = nullptr;
uint16_t sfbWalk1Width = 0;
uint16_t sfbWalk1Height = 0;
const uint16_t* sfbWalk2 = nullptr;
uint16_t sfbWalk2Width = 0;
uint16_t sfbWalk2Height = 0;
const uint16_t* sfbWalk3 = nullptr;
uint16_t sfbWalk3Width = 0;
uint16_t sfbWalk3Height = 0;
const uint16_t* sfbWalk4 = nullptr;
uint16_t sfbWalk4Width = 0;
uint16_t sfbWalk4Height = 0;
const uint16_t* sfbWalk5 = nullptr;
uint16_t sfbWalk5Width = 0;
uint16_t sfbWalk5Height = 0;
const uint16_t* sfbWalk6 = nullptr;
uint16_t sfbWalk6Width = 0;
uint16_t sfbWalk6Height = 0;
const uint16_t* sfbWalk7 = nullptr;
uint16_t sfbWalk7Width = 0;
uint16_t sfbWalk7Height = 0;
const uint16_t* sfbDuck = nullptr;
uint16_t sfbDuckWidth = 0;
uint16_t sfbDuckHeight = 0;
const uint16_t* sfbDive = nullptr;
uint16_t sfbDiveWidth = 0;
uint16_t sfbDiveHeight = 0;
const uint16_t* sfbGorilla = nullptr;
uint16_t sfbGorillaWidth = 0;
uint16_t sfbGorillaHeight = 0;
const uint16_t* sfbRedGorilla = nullptr;
uint16_t sfbRedGorillaWidth = 0;
uint16_t sfbRedGorillaHeight = 0;
const uint16_t* sfbDirt = nullptr;
uint16_t sfbDirtWidth = 0;
uint16_t sfbDirtHeight = 0;
const uint16_t* sfbDirtShadow = nullptr;
uint16_t sfbDirtShadowWidth = 0;
uint16_t sfbDirtShadowHeight = 0;
const uint16_t* sfbGrass = nullptr;
uint16_t sfbGrassWidth = 0;
uint16_t sfbGrassHeight = 0;
const uint16_t* sfbBox = nullptr;
uint16_t sfbBoxWidth = 0;
uint16_t sfbBoxHeight = 0;
const uint16_t* sfbMystery = nullptr;
uint16_t sfbMysteryWidth = 0;
uint16_t sfbMysteryHeight = 0;
const uint16_t* sfbMysteryOff = nullptr;
uint16_t sfbMysteryOffWidth = 0;
uint16_t sfbMysteryOffHeight = 0;
const uint16_t* sfbEgg = nullptr;
uint16_t sfbEggWidth = 0;
uint16_t sfbEggHeight = 0;
const uint16_t* sfbBlueEgg = nullptr;
uint16_t sfbBlueEggWidth = 0;
uint16_t sfbBlueEggHeight = 0;
const uint16_t* sfbGreenEgg = nullptr;
uint16_t sfbGreenEggWidth = 0;
uint16_t sfbGreenEggHeight = 0;
const uint16_t* sfbRat = nullptr;
uint16_t sfbRatWidth = 0;
uint16_t sfbRatHeight = 0;
const uint16_t* sfbFox = nullptr;
uint16_t sfbFoxWidth = 0;
uint16_t sfbFoxHeight = 0;
const uint16_t* sfbNest = nullptr;
uint16_t sfbNestWidth = 0;
uint16_t sfbNestHeight = 0;
const uint16_t* sfbEggCracking = nullptr;
uint16_t sfbEggCrackingWidth = 0;
uint16_t sfbEggCrackingHeight = 0;
const uint16_t* sfbEggOpen = nullptr;
uint16_t sfbEggOpenWidth = 0;
uint16_t sfbEggOpenHeight = 0;
const uint16_t* sfbGoldEgg = nullptr;
uint16_t sfbGoldEggWidth = 0;
uint16_t sfbGoldEggHeight = 0;
const uint16_t* sfbInfinityEgg = nullptr;
uint16_t sfbInfinityEggWidth = 0;
uint16_t sfbInfinityEggHeight = 0;
const uint16_t* sfbBean = nullptr;
uint16_t sfbBeanWidth = 0;
uint16_t sfbBeanHeight = 0;
const uint16_t* sfbArrowRight = nullptr;
uint16_t sfbArrowRightWidth = 0;
uint16_t sfbArrowRightHeight = 0;
const uint16_t* sfbArrowLeft = nullptr;
uint16_t sfbArrowLeftWidth = 0;
uint16_t sfbArrowLeftHeight = 0;
const uint16_t* sfbArrowUp = nullptr;
uint16_t sfbArrowUpWidth = 0;
uint16_t sfbArrowUpHeight = 0;
const uint16_t* sfbArrowDown = nullptr;
uint16_t sfbArrowDownWidth = 0;
uint16_t sfbArrowDownHeight = 0;
const uint16_t* sfbGrassyArrowRight = nullptr;
uint16_t sfbGrassyArrowRightWidth = 0;
uint16_t sfbGrassyArrowRightHeight = 0;
const uint16_t* sfbGrassyArrowLeft = nullptr;
uint16_t sfbGrassyArrowLeftWidth = 0;
uint16_t sfbGrassyArrowLeftHeight = 0;
const uint16_t* sfbGrassyArrowUp = nullptr;
uint16_t sfbGrassyArrowUpWidth = 0;
uint16_t sfbGrassyArrowUpHeight = 0;
const uint16_t* sfbGrassyArrowDown = nullptr;
uint16_t sfbGrassyArrowDownWidth = 0;
uint16_t sfbGrassyArrowDownHeight = 0;
const uint16_t* sfbCloud = nullptr;
uint16_t sfbCloudWidth = 0;
uint16_t sfbCloudHeight = 0;
const uint16_t* sfbCoin = nullptr;
uint16_t sfbCoinWidth = 0;
uint16_t sfbCoinHeight = 0;
const uint16_t* sfbYellowGrass = nullptr;
uint16_t sfbYellowGrassWidth = 0;
uint16_t sfbYellowGrassHeight = 0;
const uint16_t* sfbCloudBackground = nullptr;
uint16_t sfbCloudBackgroundWidth = 0;
uint16_t sfbCloudBackgroundHeight = 0;
const uint16_t* sfbCardboardBox = nullptr;
uint16_t sfbCardboardBoxWidth = 0;
uint16_t sfbCardboardBoxHeight = 0;
const uint16_t* sfbTennisBall = nullptr;
uint16_t sfbTennisBallWidth = 0;
uint16_t sfbTennisBallHeight = 0;
const uint16_t* sfbWhiteArrowRight = nullptr;
uint16_t sfbWhiteArrowRightWidth = 0;
uint16_t sfbWhiteArrowRightHeight = 0;
const uint16_t* sfbWhiteArrowLeft = nullptr;
uint16_t sfbWhiteArrowLeftWidth = 0;
uint16_t sfbWhiteArrowLeftHeight = 0;
const uint16_t* sfbWhiteArrowUp = nullptr;
uint16_t sfbWhiteArrowUpWidth = 0;
uint16_t sfbWhiteArrowUpHeight = 0;
const uint16_t* sfbWhiteArrowDown = nullptr;
uint16_t sfbWhiteArrowDownWidth = 0;
uint16_t sfbWhiteArrowDownHeight = 0;
const uint16_t* golfEggUp = nullptr;
uint16_t golfEggUpWidth = 0;
uint16_t golfEggUpHeight = 0;
const uint16_t* golfEggDown = nullptr;
uint16_t golfEggDownWidth = 0;
uint16_t golfEggDownHeight = 0;
const uint16_t* sfbSkyRows = nullptr;
const uint8_t* sfbSkyRuns = nullptr;

void bindSuperFlashyArt(const SuperFlashyAssets& assets) {
  setArt(assets.bird, sfbBird, sfbBirdWidth, sfbBirdHeight);
  setArt(assets.walk[0], sfbWalk0, sfbWalk0Width, sfbWalk0Height);
  setArt(assets.walk[1], sfbWalk1, sfbWalk1Width, sfbWalk1Height);
  setArt(assets.walk[2], sfbWalk2, sfbWalk2Width, sfbWalk2Height);
  setArt(assets.walk[3], sfbWalk3, sfbWalk3Width, sfbWalk3Height);
  setArt(assets.walk[4], sfbWalk4, sfbWalk4Width, sfbWalk4Height);
  setArt(assets.walk[5], sfbWalk5, sfbWalk5Width, sfbWalk5Height);
  setArt(assets.walk[6], sfbWalk6, sfbWalk6Width, sfbWalk6Height);
  setArt(assets.walk[7], sfbWalk7, sfbWalk7Width, sfbWalk7Height);
  setArt(assets.duck, sfbDuck, sfbDuckWidth, sfbDuckHeight);
  setArt(assets.dive, sfbDive, sfbDiveWidth, sfbDiveHeight);
  setArt(assets.gorilla, sfbGorilla, sfbGorillaWidth, sfbGorillaHeight);
  setArt(assets.redGorilla, sfbRedGorilla, sfbRedGorillaWidth,
         sfbRedGorillaHeight);
  setArt(assets.dirt, sfbDirt, sfbDirtWidth, sfbDirtHeight);
  setArt(assets.dirtShadow, sfbDirtShadow, sfbDirtShadowWidth,
         sfbDirtShadowHeight);
  setArt(assets.grass, sfbGrass, sfbGrassWidth, sfbGrassHeight);
  setArt(assets.box, sfbBox, sfbBoxWidth, sfbBoxHeight);
  setArt(assets.mystery, sfbMystery, sfbMysteryWidth, sfbMysteryHeight);
  setArt(assets.mysteryOff, sfbMysteryOff, sfbMysteryOffWidth,
         sfbMysteryOffHeight);
  setArt(assets.egg, sfbEgg, sfbEggWidth, sfbEggHeight);
  setArt(assets.blueEgg, sfbBlueEgg, sfbBlueEggWidth, sfbBlueEggHeight);
  setArt(assets.greenEgg, sfbGreenEgg, sfbGreenEggWidth, sfbGreenEggHeight);
  setArt(assets.rat, sfbRat, sfbRatWidth, sfbRatHeight);
  setArt(assets.fox, sfbFox, sfbFoxWidth, sfbFoxHeight);
  setArt(assets.nest, sfbNest, sfbNestWidth, sfbNestHeight);
  setArt(assets.eggCracking, sfbEggCracking, sfbEggCrackingWidth,
         sfbEggCrackingHeight);
  setArt(assets.eggOpen, sfbEggOpen, sfbEggOpenWidth, sfbEggOpenHeight);
  setArt(assets.goldEgg, sfbGoldEgg, sfbGoldEggWidth, sfbGoldEggHeight);
  setArt(assets.infinityEgg, sfbInfinityEgg, sfbInfinityEggWidth,
         sfbInfinityEggHeight);
  setArt(assets.bean, sfbBean, sfbBeanWidth, sfbBeanHeight);
  setArt(assets.arrowRight, sfbArrowRight, sfbArrowRightWidth,
         sfbArrowRightHeight);
  setArt(assets.arrowLeft, sfbArrowLeft, sfbArrowLeftWidth, sfbArrowLeftHeight);
  setArt(assets.arrowUp, sfbArrowUp, sfbArrowUpWidth, sfbArrowUpHeight);
  setArt(assets.arrowDown, sfbArrowDown, sfbArrowDownWidth, sfbArrowDownHeight);
  setArt(assets.grassyArrowRight, sfbGrassyArrowRight, sfbGrassyArrowRightWidth,
         sfbGrassyArrowRightHeight);
  setArt(assets.grassyArrowLeft, sfbGrassyArrowLeft, sfbGrassyArrowLeftWidth,
         sfbGrassyArrowLeftHeight);
  setArt(assets.grassyArrowUp, sfbGrassyArrowUp, sfbGrassyArrowUpWidth,
         sfbGrassyArrowUpHeight);
  setArt(assets.grassyArrowDown, sfbGrassyArrowDown, sfbGrassyArrowDownWidth,
         sfbGrassyArrowDownHeight);
  setArt(assets.cloud, sfbCloud, sfbCloudWidth, sfbCloudHeight);
  setArt(assets.coin, sfbCoin, sfbCoinWidth, sfbCoinHeight);
  setArt(assets.yellowGrass, sfbYellowGrass, sfbYellowGrassWidth,
         sfbYellowGrassHeight);
  setArt(assets.cloudBackground, sfbCloudBackground, sfbCloudBackgroundWidth,
         sfbCloudBackgroundHeight);
  setArt(assets.cardboardBox, sfbCardboardBox, sfbCardboardBoxWidth,
         sfbCardboardBoxHeight);
  setArt(assets.tennisBall, sfbTennisBall, sfbTennisBallWidth,
         sfbTennisBallHeight);
  setArt(assets.whiteArrowRight, sfbWhiteArrowRight, sfbWhiteArrowRightWidth,
         sfbWhiteArrowRightHeight);
  setArt(assets.whiteArrowLeft, sfbWhiteArrowLeft, sfbWhiteArrowLeftWidth,
         sfbWhiteArrowLeftHeight);
  setArt(assets.whiteArrowUp, sfbWhiteArrowUp, sfbWhiteArrowUpWidth,
         sfbWhiteArrowUpHeight);
  setArt(assets.whiteArrowDown, sfbWhiteArrowDown, sfbWhiteArrowDownWidth,
         sfbWhiteArrowDownHeight);
  setArt(assets.golfEggUp, golfEggUp, golfEggUpWidth, golfEggUpHeight);
  setArt(assets.golfEggDown, golfEggDown, golfEggDownWidth, golfEggDownHeight);
  sfbSkyRows = nullptr;
  sfbSkyRuns = nullptr;
  if (assets.sky.data != nullptr && assets.sky.size >= 6 + sfbSkyHeight * 2) {
    const uint16_t width = assets.sky.data[0] | (assets.sky.data[1] << 8);
    const uint16_t height = assets.sky.data[2] | (assets.sky.data[3] << 8);
    if (width == sfbSkyWidth && height == sfbSkyHeight) {
      sfbSkyRows = reinterpret_cast<const uint16_t*>(assets.sky.data + 6);
      sfbSkyRuns = assets.sky.data + 6 + height * 2;
    }
  }
}

bool loadSuperFlashyMenuArt(SuperFlashyAssets& assets) {
  assets.release();
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  const auto load = [&](const char* path, const char* format,
                        GameAssetImage& image) {
    return loadOneImage(path, format, 0, 0, image, entries, count);
  };
  // The sky is large. The tile only needs it when there is room, so a failed
  // sky download must not throw away the bird and the ground.
  const bool ok = load("super/grass.rgb565", "rgb565", assets.grass) &&
                  load("super/bird.rgb565", "rgb565", assets.bird) &&
                  load("super/walk0.rgb565", "rgb565", assets.walk[0]) &&
                  load("super/mystery.rgb565", "rgb565", assets.mystery) &&
                  load("super/box.rgb565", "rgb565", assets.box) &&
                  load("super/gold_egg.rgb565", "rgb565", assets.goldEgg) &&
                  load("super/red_gorilla.rgb565", "rgb565", assets.redGorilla);
  if (!ok) {
    assets.release();
    return false;
  }
  // The golf tile draws this egg. A missing file leaves that tile without it
  // and keeps the rest of the menu.
  load("golf/egg_up.rgb565", "rgb565", assets.golfEggUp);
  load("super/sky.rle", "rle332", assets.sky);
  return true;
}

bool switchFlappyBackground(FlappyAssets& assets, bool night) {
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  return loadOneImage(night ? "flappy/background_night.rgb332"
                            : "flappy/background_day.rgb332",
                      "rgb332", 320, 240, assets.background, entries, count);
}

bool loadDeskSpotifyLogo(GameAssetImage& logo) {
  logo.release();
  const ManifestEntries owned = makeEntries();
  ManifestEntry* const entries = owned.get();
  if (entries == nullptr) return false;
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  return loadOneImage("desk/spotify_logo.rgb565", "rgb565", 64, 64, logo,
                      entries, count);
}

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

#include "device_config.h"

namespace {

constexpr char CACHE_ROOT[] = "/game_assets";
constexpr char MANIFEST_PATH[] = "/game_assets/manifest.txt";
constexpr char MANIFEST_PART_PATH[] = "/game_assets/manifest.txt.part";
constexpr char MANIFEST_BACKUP_PATH[] = "/game_assets/manifest.txt.bak";
constexpr uint8_t MAX_MANIFEST_ENTRIES = 40;
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
  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
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

  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
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
  SuperFlashyAssets superAssets;
  const bool succeeded = initGameAssets() &&
                         loadFlappyAssets(flappyAssets) &&
                         loadSuperFlashyAssets(superAssets);
  if (succeeded) {
    lockFilesystem();
    preloadedFlappyAssets.release();
    preloadedSuperFlashyAssets.release();
    preloadedFlappyAssets = flappyAssets;
    preloadedSuperFlashyAssets = superAssets;
    flappyAssets = FlappyAssets{};
    superAssets = SuperFlashyAssets{};
    assetMemoryPreloadReady = true;
    unlockFilesystem();
  } else {
    flappyAssets.release();
    superAssets.release();
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
      strcmp(entry.format, format) != 0 || entry.width != width ||
      entry.height != height) {
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
  gorilla.release();
  redGorilla.release();
  dirt.release();
  grass.release();
  box.release();
  mystery.release();
  mysteryOff.release();
  egg.release();
  blueEgg.release();
  rat.release();
  fox.release();
  nest.release();
  eggCracking.release();
  eggOpen.release();
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

bool loadFlappyAssets(FlappyAssets& assets, bool night) {
  assets.release();
  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;

  bool ok =
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
                   assets.pipeCap, entries, count) &&
      loadOneImage(night ? "flappy/background_night.rgb332"
                         : "flappy/background_day.rgb332",
                   "rgb332", 320, 240, assets.background, entries, count);
  if (!ok) assets.release();
  return ok;
}

bool loadSuperFlashyAssets(SuperFlashyAssets& assets) {
  assets.release();
  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;

  bool ok =
      loadOneImage("super/bird.rgb565", "rgb565", 16, 32, assets.bird,
                   entries, count) &&
      loadOneImage("super/walk0.rgb565", "rgb565", 32, 32, assets.walk[0],
                   entries, count) &&
      loadOneImage("super/walk1.rgb565", "rgb565", 32, 32, assets.walk[1],
                   entries, count) &&
      loadOneImage("super/walk2.rgb565", "rgb565", 32, 32, assets.walk[2],
                   entries, count) &&
      loadOneImage("super/walk3.rgb565", "rgb565", 32, 32, assets.walk[3],
                   entries, count) &&
      loadOneImage("super/walk4.rgb565", "rgb565", 32, 32, assets.walk[4],
                   entries, count) &&
      loadOneImage("super/walk5.rgb565", "rgb565", 32, 32, assets.walk[5],
                   entries, count) &&
      loadOneImage("super/walk6.rgb565", "rgb565", 32, 32, assets.walk[6],
                   entries, count) &&
      loadOneImage("super/walk7.rgb565", "rgb565", 32, 32, assets.walk[7],
                   entries, count) &&
      loadOneImage("super/duck.rgb565", "rgb565", 31, 24, assets.duck,
                   entries, count) &&
      loadOneImage("super/gorilla.rgb565", "rgb565", 23, 20, assets.gorilla,
                   entries, count) &&
      loadOneImage("super/red_gorilla.rgb565", "rgb565", 23, 20,
                   assets.redGorilla, entries, count) &&
      loadOneImage("super/dirt.rgb565", "rgb565", 16, 16, assets.dirt,
                   entries, count) &&
      loadOneImage("super/grass.rgb565", "rgb565", 16, 16, assets.grass,
                   entries, count) &&
      loadOneImage("super/box.rgb565", "rgb565", 16, 16, assets.box,
                   entries, count) &&
      loadOneImage("super/mystery.rgb565", "rgb565", 16, 16, assets.mystery,
                   entries, count) &&
      loadOneImage("super/mystery_off.rgb565", "rgb565", 16, 16,
                   assets.mysteryOff, entries, count) &&
      loadOneImage("super/egg.rgb565", "rgb565", 16, 16, assets.egg,
                   entries, count) &&
      loadOneImage("super/blue_egg.rgb565", "rgb565", 16, 16,
                   assets.blueEgg, entries, count) &&
      loadOneImage("super/rat.rgb565", "rgb565", 24, 10, assets.rat,
                   entries, count) &&
      loadOneImage("super/fox.rgb565", "rgb565", 35, 24, assets.fox,
                   entries, count) &&
      loadOneImage("super/nest.rgb565", "rgb565", 40, 23, assets.nest,
                   entries, count) &&
      loadOneImage("super/egg_cracking.rgb565", "rgb565", 16, 16,
                   assets.eggCracking, entries, count) &&
      loadOneImage("super/egg_open.rgb565", "rgb565", 16, 16,
                   assets.eggOpen, entries, count);
  if (!ok) assets.release();
  return ok;
}

bool switchFlappyBackground(FlappyAssets& assets, bool night) {
  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  return loadOneImage(night ? "flappy/background_night.rgb332"
                            : "flappy/background_day.rgb332",
                      "rgb332", 320, 240, assets.background, entries, count);
}

bool loadDeskSpotifyLogo(GameAssetImage& logo) {
  logo.release();
  static ManifestEntry entries[MAX_MANIFEST_ENTRIES];
  size_t count = 0;
  if (!loadManifestEntries(entries, count)) return false;
  return loadOneImage("desk/spotify_logo.rgb565", "rgb565", 64, 64, logo,
                      entries, count);
}

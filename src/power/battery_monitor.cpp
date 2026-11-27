#include "power/battery_monitor.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "game_assets.h"

namespace {
constexpr uint8_t BATTERY_ADC_PIN = 34;
constexpr uint8_t BATTERY_SAMPLE_COUNT = 16;
constexpr uint32_t BATTERY_PERIOD_MS = 60UL * 1000UL;
// Close to empty the battery is checked more often so the cutoff is not
// missed by a full minute.
constexpr uint32_t BATTERY_LOW_PERIOD_MS = 10UL * 1000UL;
constexpr uint16_t BATTERY_LOW_WATCH_MV = 3500;
constexpr uint8_t CUTOFF_CONFIRM_READINGS = 2;
constexpr uint32_t CUTOFF_CONFIRM_GAP_MS = 1000;
constexpr uint16_t BATTERY_MIN_VALID_MV = 2500;
constexpr char BATTERY_LOG_PATH[] = "/battery_log.csv";

SemaphoreHandle_t adcMutex = nullptr;
TaskHandle_t batteryTaskHandle = nullptr;
volatile bool batteryPaused = false;
volatile bool cutoffShutdownStarted = false;
void (*cutoffHandler)() = nullptr;
bool pinReady = false;
int8_t currentPercent = -1;
uint32_t smoothedMilliVolts = 0;

void preparePin() {
  if (pinReady) return;
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  analogReadMilliVolts(BATTERY_ADC_PIN);  // the first read after setup is off
  pinReady = true;
}

// The same 3.36 V line drives the 0% readout, the power-on check, and the
// shutdown. Zero means no reading, so an unconnected battery never counts.
bool underCutoff(uint32_t mv) {
  return mv > 0 && mv <= BATTERY_CUTOFF_MV;
}

uint32_t readMilliVolts() {
  batteryAdcAcquire();
  uint32_t total = 0;
  for (uint8_t sample = 0; sample < BATTERY_SAMPLE_COUNT; ++sample) {
    total += analogReadMilliVolts(BATTERY_ADC_PIN);
  }
  batteryAdcRelease();
  return (total / BATTERY_SAMPLE_COUNT) * 2;
}

int8_t percentFromMilliVolts(uint32_t mv) {
  struct Point {
    uint16_t mv;
    uint8_t percent;
  };
  static const Point CURVE[] = {
      {4200, 100}, {4150, 95}, {4100, 90}, {4050, 85}, {4000, 80},
      {3950, 70},  {3900, 60}, {3850, 50}, {3800, 40}, {3750, 30},
      {3700, 20},  {3650, 15}, {3600, 10}, {3500, 5},  {3400, 2},
      {3360, 0},
  };
  constexpr size_t COUNT = sizeof(CURVE) / sizeof(CURVE[0]);
  // 3.36 V and below is 0%. A fraction of a percent just above that still
  // shows 1%, so the gauge only reads 0% at the bottom of the curve.
  if (mv <= CURVE[COUNT - 1].mv) return 0;
  if (mv >= CURVE[0].mv) return 100;
  for (size_t index = 1; index < COUNT; ++index) {
    if (mv >= CURVE[index].mv) {
      const uint32_t span = CURVE[index - 1].mv - CURVE[index].mv;
      const uint32_t rise = CURVE[index - 1].percent - CURVE[index].percent;
      const int32_t value = CURVE[index].percent +
                            static_cast<int32_t>(((mv - CURVE[index].mv) * rise) / span);
      return static_cast<int8_t>(value < 1 ? 1 : value);
    }
  }
  return 1;
}

// Takes one reading and folds it into the smoothed value.
void sampleBattery() {
  const uint32_t mv = readMilliVolts();
  if (mv < BATTERY_MIN_VALID_MV) {
    // Once a battery has been read, a sub-2.5 V reading means it has died
    // (or was unplugged). Show 0% so the cutoff takes it. Before any reading
    // it is just no battery.
    if (smoothedMilliVolts != 0) {
      smoothedMilliVolts = mv;
      currentPercent = mv == 0 ? -1 : 0;
    }
    return;
  }
  smoothedMilliVolts =
      smoothedMilliVolts == 0 ? mv : (smoothedMilliVolts * 3 + mv) / 4;
  currentPercent = percentFromMilliVolts(smoothedMilliVolts);
}

// A single low reading can be a dip from Wi-Fi or audio. The smoothed value
// has to stay under the cutoff across a few more readings a second apart.
bool cutoffConfirmed() {
  for (uint8_t reading = 0; reading < CUTOFF_CONFIRM_READINGS; ++reading) {
    vTaskDelay(pdMS_TO_TICKS(CUTOFF_CONFIRM_GAP_MS));
    sampleBattery();
    if (!underCutoff(smoothedMilliVolts)) return false;
  }
  return true;
}

void appendLogLine() {
  if (currentPercent < 0) return;
  if (!lockGameAssetsFilesystem(200)) return;
  File file = LittleFS.open(BATTERY_LOG_PATH, FILE_APPEND);
  if (file) {
    file.printf("%lu,%lu,%d\n", static_cast<unsigned long>(millis()),
                static_cast<unsigned long>(smoothedMilliVolts),
                currentPercent);
    file.close();
  }
  unlockGameAssetsFilesystem();
}

void batteryTask(void*) {
  preparePin();
  sampleBattery();
  while (true) {
    const bool nearEmpty =
        smoothedMilliVolts != 0 && smoothedMilliVolts < BATTERY_LOW_WATCH_MV;
    vTaskDelay(pdMS_TO_TICKS(nearEmpty ? BATTERY_LOW_PERIOD_MS
                                       : BATTERY_PERIOD_MS));
    if (batteryPaused) continue;
    sampleBattery();
    if (!cutoffShutdownStarted && cutoffHandler != nullptr &&
        underCutoff(smoothedMilliVolts) && cutoffConfirmed()) {
      cutoffShutdownStarted = true;
      cutoffHandler();
    }
  }
}
}  // namespace

void batteryAdcAcquire() {
  if (adcMutex != nullptr) {
    xSemaphoreTake(adcMutex, portMAX_DELAY);
  }
}

void batteryAdcRelease() {
  if (adcMutex != nullptr) {
    xSemaphoreGive(adcMutex);
  }
}

void startBatteryMonitor() {
  if (batteryTaskHandle != nullptr) return;
  if (adcMutex == nullptr) adcMutex = xSemaphoreCreateMutex();
  // The cutoff handler powers the device off from this task, and that path
  // writes the battery log, so it needs more than the bare minimum.
  xTaskCreatePinnedToCore(batteryTask, "battery", 6144, nullptr, 1,
                          &batteryTaskHandle, 0);
}

bool batteryBelowCutoff() {
  preparePin();
  return underCutoff(readMilliVolts());
}

void setBatteryCutoffHandler(void (*handler)()) { cutoffHandler = handler; }

void pauseBatteryLogging() { batteryPaused = true; }

void batteryLogSessionEnd() {
  sampleBattery();
  appendLogLine();
}

int8_t batteryPercent() { return currentPercent; }

uint16_t batteryMilliVolts() {
  return static_cast<uint16_t>(smoothedMilliVolts);
}

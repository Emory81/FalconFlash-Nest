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
constexpr uint16_t BATTERY_MIN_VALID_MV = 2500;
constexpr char BATTERY_LOG_PATH[] = "/battery_log.csv";

SemaphoreHandle_t adcMutex = nullptr;
TaskHandle_t batteryTaskHandle = nullptr;
volatile bool batteryPaused = false;
int8_t currentPercent = -1;
uint32_t smoothedMilliVolts = 0;

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
      {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80},
      {3980, 75},  {3950, 70}, {3910, 65}, {3870, 60}, {3850, 55},
      {3840, 50},  {3820, 45}, {3800, 40}, {3790, 35}, {3770, 30},
      {3750, 25},  {3730, 20}, {3710, 15}, {3690, 10}, {3610, 5},
      {3270, 0},
  };
  constexpr size_t COUNT = sizeof(CURVE) / sizeof(CURVE[0]);
  if (mv >= CURVE[0].mv) return 100;
  if (mv <= CURVE[COUNT - 1].mv) return 0;
  for (size_t index = 1; index < COUNT; ++index) {
    if (mv >= CURVE[index].mv) {
      const uint32_t span = CURVE[index - 1].mv - CURVE[index].mv;
      const uint32_t rise = CURVE[index - 1].percent - CURVE[index].percent;
      return static_cast<int8_t>(CURVE[index].percent +
                                 ((mv - CURVE[index].mv) * rise) / span);
    }
  }
  return 0;
}

void sampleBattery() {
  const uint32_t mv = readMilliVolts();
  if (mv < BATTERY_MIN_VALID_MV) return;
  smoothedMilliVolts =
      smoothedMilliVolts == 0 ? mv : (smoothedMilliVolts * 3 + mv) / 4;
  currentPercent = percentFromMilliVolts(smoothedMilliVolts);
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
  pinMode(BATTERY_ADC_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  sampleBattery();
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(BATTERY_PERIOD_MS));
    if (batteryPaused) continue;
    sampleBattery();
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
  xTaskCreatePinnedToCore(batteryTask, "battery", 4096, nullptr, 1,
                          &batteryTaskHandle, 0);
}

void pauseBatteryLogging() { batteryPaused = true; }

void batteryLogSessionEnd() {
  sampleBattery();
  appendLogLine();
}

int8_t batteryPercent() { return currentPercent; }

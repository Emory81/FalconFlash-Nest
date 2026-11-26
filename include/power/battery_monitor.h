#pragma once

#include <stdint.h>

// GPIO34 reads half the battery voltage. These calls share that ADC with the
// button ladder so the two never sample at the same time.
void startBatteryMonitor();
void pauseBatteryLogging();
void batteryLogSessionEnd();
int8_t batteryPercent();
void batteryAdcAcquire();
void batteryAdcRelease();

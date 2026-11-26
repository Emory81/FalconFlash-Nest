#pragma once

#include <stdint.h>

// GPIO34 reads half the battery voltage. These calls share that ADC with the
// button ladder so the two never sample at the same time.
void startBatteryMonitor();
void pauseBatteryLogging();
void batteryLogSessionEnd();
int8_t batteryPercent();

// Below this the battery counts as empty (0%). The device refuses to start
// under it and shuts itself off if it drops under it while running. Readings
// under 2.5 V are taken to mean no battery is connected (USB power only).
constexpr uint16_t BATTERY_CUTOFF_MV = 3000;

// Measure right now and report whether a connected battery is under the
// cutoff. Safe to call before the monitor task has started.
bool batteryBelowCutoff();

// Called from the monitor task when the battery stays under the cutoff. The
// handler should power the device off.
void setBatteryCutoffHandler(void (*handler)());
void batteryAdcAcquire();
void batteryAdcRelease();

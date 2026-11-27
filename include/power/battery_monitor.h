#pragma once

#include <stdint.h>

// GPIO34 reads half the battery voltage. These calls share that ADC with the
// button ladder so the two never sample at the same time.
void startBatteryMonitor();
void pauseBatteryLogging();
void batteryLogSessionEnd();
int8_t batteryPercent();

// Smoothed battery voltage in millivolts at the battery terminals. Zero means
// no reading yet.
uint16_t batteryMilliVolts();

// At or below this the gauge reads 0%, the device refuses to start, and it
// shuts itself off once the reading holds.
constexpr uint16_t BATTERY_CUTOFF_MV = 3360;

// Measure right now and report whether a connected battery is under the
// cutoff. Safe to call before the monitor task has started.
bool batteryBelowCutoff();

// Called from the monitor task when the battery stays under the cutoff. The
// handler should power the device off.
void setBatteryCutoffHandler(void (*handler)());
void batteryAdcAcquire();
void batteryAdcRelease();

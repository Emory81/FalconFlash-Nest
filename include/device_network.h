#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <TFT_eSPI.h>

void startDeviceWiFi();
bool ensureDevicePaired(TFT_eSPI& display);
enum class DevicePriceStatus : uint8_t {
  Success,
  Unavailable,
  NeedsPairing,
};
DevicePriceStatus fetchDevicePrice(uint8_t buttonNumber, String& priceText);
bool submitDeviceOrder(uint8_t buttonNumber);

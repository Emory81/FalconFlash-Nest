#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <TFT_eSPI.h>

void startDeviceWiFi();
bool deviceWiFiConnected();
bool checkForFirmwareUpdate();
bool ensureDevicePaired(TFT_eSPI& display);
enum class DevicePriceStatus : uint8_t {
  Success,
  Unavailable,
  Unarmed,
  NeedsPairing,
};

struct OrderStatus {
  String orderStatus;
  String orderNumber;
  bool orderPlaced;
  String orderPlacedAt;
  String customerOrderPlacedAt;
  bool flasherAssigned;
  String flasherName;
  String pickedUpAt;
  String deliveredAt;
  String orderPhoto;
};

DevicePriceStatus fetchDevicePrice(uint8_t buttonNumber, String& priceText,
                                   uint8_t& savedPickupLocationMask);
bool submitDeviceOrder(uint8_t buttonNumber, uint8_t pickupLocationMask,
                       String& eventId, String& orderId);
bool fetchOrderStatus(const String& orderId, OrderStatus& status);

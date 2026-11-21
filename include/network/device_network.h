#pragma once

#include <Arduino.h>
#include <stdint.h>

#include <TFT_eSPI.h>

void startDeviceWiFi();
bool deviceWiFiConnected();
bool checkForFirmwareUpdate(TFT_eSPI& display, uint8_t wakeButtonNumber);
uint8_t takeOtaResumeButton();
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
  bool canceledOrRefunded;
  String orderPhoto;
};

DevicePriceStatus fetchDevicePrice(uint8_t buttonNumber, String& priceText,
                                   uint8_t& savedPickupLocationMask);
enum class DeviceOrderStatus : uint8_t {
  Success,
  Unavailable,
  Unarmed,
  NeedsPairing,
  InvalidResponse,
};

DeviceOrderStatus submitDeviceOrder(uint8_t buttonNumber,
                                    uint8_t pickupLocationMask,
                                    String& eventId, String& orderId,
                                    String& failureDetail);
bool fetchOrderStatus(const String& orderId, OrderStatus& status);

enum class DeviceMessagePollStatus : uint8_t {
  Success,
  Unavailable,
  NeedsPairing,
};

struct DeviceMessage {
  uint32_t messageId = 0;
  uint8_t buttonNumber = 0;
  String flyerName;
  String message;
  String createdAt;
};

DeviceMessagePollStatus fetchDeviceMessages(
    uint8_t buttonNumber, uint32_t sinceMessageId, DeviceMessage* messages,
    uint8_t maxMessages, uint8_t& messageCount,
    uint32_t& nextSinceMessageId);

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdio>

#include "audio/audio_helper.h"
#include "audio/boot_sound.h"
#include "audio/delivery_sound.h"
#include "audio/power_down_sound.h"
#include "device_config.h"
#include "display/display_config.h"
#include "display/falcon_logo.h"
#include "game_assets.h"
#include "games/game_menu.h"
#include "input/button_input.h"
#include "network/device_network.h"

namespace {
constexpr uint8_t BACKLIGHT_PIN = 21;
constexpr uint8_t BUTTON_WAKE_PIN = 35;
constexpr int16_t LOGO_TEXT_GAP = 4;
constexpr uint32_t DEMO_HOLD_MS = 1000;
constexpr uint32_t COUNTDOWN_DURATION_MS = 15000;
constexpr int16_t TIMER_MAX_RADIUS = 52;
constexpr int16_t TIMER_RING_THICKNESS = 6;
constexpr int16_t TIMER_CENTER_Y = 122;
constexpr int16_t CANCEL_HINT_BOTTOM_MARGIN = 10;
constexpr uint16_t ARC_TOP_ANGLE = 180;  // 12 o'clock in TFT_eSPI arc coordinates
constexpr uint32_t THANK_YOU_SCROLL_IN_MS = 500;
constexpr uint32_t THANK_YOU_HOLD_MS = 1000;
constexpr uint32_t THANK_YOU_SCROLL_OUT_MS = 500;
constexpr uint16_t THANK_YOU_FRAME_MS = 20;
constexpr uint32_t ORDER_STATUS_POLL_MS = 2000;
constexpr uint32_t ORDER_STATUS_HOLD_MS = 2000;
constexpr uint32_t ORDER_STATUS_SCROLL_MS = 450;
constexpr uint16_t ORDER_STATUS_FRAME_MS = 20;
constexpr int16_t ORDER_TIMELINE_VIEW_TOP = 44;
constexpr int16_t ORDER_TIMELINE_FIRST_Y = 68;
constexpr int16_t ORDER_TIMELINE_ROW_HEIGHT = 58;
constexpr int16_t ORDER_TIMELINE_NODE_X = 28;
constexpr int16_t ORDER_TIMELINE_TEXT_X = 58;
constexpr int16_t ORDER_TIMELINE_NODE_RADIUS = 16;
constexpr uint16_t ORDER_BG = 0x18C7;
constexpr uint16_t ORDER_BLUE = 0x3D8F;
constexpr uint16_t ORDER_SUBTEXT = 0xB5C7;
constexpr int16_t GAME_BUTTON_X = 8;
constexpr int16_t GAME_BUTTON_Y = 208;
constexpr int16_t GAME_BUTTON_W = 66;
constexpr int16_t GAME_BUTTON_H = 25;
constexpr uint8_t DELIVERY_SOUND_REPEATS = 3;
constexpr uint32_t DELIVERY_SOUND_GAP_MS = 1000;
constexpr uint32_t POST_DELIVERY_STANDBY_MS = 15000;
constexpr uint8_t PRICE_FETCH_RETRIES = 3;
constexpr uint32_t PRICE_FETCH_RETRY_DELAY_MS = 500;
constexpr uint8_t ORDER_SUBMIT_RETRIES = 3;
constexpr uint8_t PICKUP_LOCATION_COUNT = 5;
constexpr int16_t PICKUP_MENU_TILE_WIDTH = 148;
constexpr int16_t PICKUP_MENU_TILE_HEIGHT = 48;
constexpr int16_t PICKUP_MENU_GAP = 8;
constexpr int16_t PICKUP_MENU_LEFT = 8;
constexpr int16_t PICKUP_MENU_TOP = 42;
constexpr uint32_t MESSAGE_NOTIFICATION_DURATION_MS = 5000;
constexpr uint8_t MAX_MESSAGE_NOTIFICATIONS = 4;
constexpr uint8_t MESSAGE_POLL_LIMIT = 20;
constexpr int16_t MESSAGE_PANEL_WIDTH = 296;

struct PickupLocationDisplay {
  const char* shortName;
  const char* lineOne;
  const char* lineTwo;
};

constexpr PickupLocationDisplay PICKUP_LOCATIONS[PICKUP_LOCATION_COUNT] = {
    {"Left Side", "Milkshakes, Acai", "bowls, etc."},
    {"Middle", "Burgers etc.", ""},
    {"Right Side", "Pasta etc.", ""},
    {"Back Side", "Coffee, Specialty", "drinks etc."},
    {"uCreate", "", ""},
};

struct MessageNotificationState {
  DeviceMessage messages[MAX_MESSAGE_NOTIFICATIONS];
  uint8_t count = 0;
  uint32_t expiresAt = 0;
};

struct MessagePollWorker {
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t mutex = nullptr;
  volatile bool stopRequested = false;
  volatile bool finished = false;
  volatile bool paused = false;
  bool resultReady = false;
  DeviceMessage messages[MESSAGE_POLL_LIMIT];
  uint8_t messageCount = 0;
};

MessagePollWorker* activeMessagePollWorker = nullptr;

TFT_eSPI tft;

void drawSplashScreen() {
  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();

  tft.fillScreen(TFT_BLACK);

  tft.setTextFont(1);
  tft.setTextSize(2);
  const int16_t textHeight = tft.fontHeight();
  const int16_t blockHeight = FALCON_LOGO_HEIGHT + LOGO_TEXT_GAP + textHeight;

  const int16_t logoX = (screenW - FALCON_LOGO_WIDTH) / 2;
  const int16_t logoY = (screenH - blockHeight) / 2;
  tft.pushImage(logoX, logoY, FALCON_LOGO_WIDTH, FALCON_LOGO_HEIGHT, falconLogo);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(
      "FalconFlash.co", screenW / 2,
      logoY + FALCON_LOGO_HEIGHT + LOGO_TEXT_GAP + textHeight / 2);
}

String pickupLocationSummary(uint8_t pickupLocationMask) {
  if (pickupLocationMask == 0) {
    return String("Saved default");
  }

  String summary;
  for (uint8_t index = 0; index < PICKUP_LOCATION_COUNT; ++index) {
    if ((pickupLocationMask & (1U << index)) == 0) {
      continue;
    }
    if (summary.length() > 0) {
      summary += " + ";
    }
    summary += PICKUP_LOCATIONS[index].shortName;
  }
  return summary.length() > 0 ? summary : String("Saved default");
}

void drawPriceHeader(int16_t centerX, const char* priceText,
                     uint8_t pickupLocationMask,
                     uint8_t savedPickupLocationMask) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(priceText, centerX, 20);
  tft.setTextSize(1);
  const uint8_t displayMask =
      pickupLocationMask != 0 ? pickupLocationMask : savedPickupLocationMask;
  const String pickupText =
      String("Pickup: ") + pickupLocationSummary(displayMask);
  tft.drawString(pickupText.c_str(), centerX, 45);
}

void drawPriceUnavailable() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Price request failed", tft.width() / 2, 92);
  tft.setTextSize(1);
  tft.drawString("Check network or device status", tft.width() / 2, 132);
}

void drawButtonUnarmed(uint8_t buttonNumber) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Button not armed", tft.width() / 2, 92);

  char message[24];
  snprintf(message, sizeof(message), "BTN%u is not armed", buttonNumber);
  tft.setTextSize(1);
  tft.drawString(message, tft.width() / 2, 132);
}

DevicePriceStatus fetchPriceWithRetry(uint8_t buttonNumber,
                                      String& priceText,
                                      uint8_t& savedPickupLocationMask) {
  DevicePriceStatus status = DevicePriceStatus::Unavailable;
  for (uint8_t attempt = 0; attempt < PRICE_FETCH_RETRIES; ++attempt) {
    status =
        fetchDevicePrice(buttonNumber, priceText, savedPickupLocationMask);
    if (status != DevicePriceStatus::Unavailable ||
        attempt + 1 >= PRICE_FETCH_RETRIES) {
      return status;
    }
    delay(PRICE_FETCH_RETRY_DELAY_MS);
  }
  return status;
}

void drawFullTimerRing(int16_t centerX, int16_t centerY) {
  const int16_t outerRadius = TIMER_MAX_RADIUS;
  const int16_t innerRadius = outerRadius - TIMER_RING_THICKNESS;
  tft.drawArc(centerX, centerY, outerRadius, innerRadius, ARC_TOP_ANGLE + 1,
              ARC_TOP_ANGLE - 1, TFT_WHITE, TFT_BLACK, false);
}

void eraseTimerArcSegment(int16_t centerX, int16_t centerY, float startDegrees,
                          float endDegrees) {
  if (endDegrees <= startDegrees) {
    return;
  }

  const int16_t outerRadius = TIMER_MAX_RADIUS;
  const int16_t innerRadius = outerRadius - TIMER_RING_THICKNESS;
  const uint16_t startAngle =
      static_cast<uint16_t>(ARC_TOP_ANGLE + startDegrees) % 360;
  const uint16_t endAngle =
      static_cast<uint16_t>(ARC_TOP_ANGLE + endDegrees) % 360;

  tft.drawArc(centerX, centerY, outerRadius, innerRadius, startAngle, endAngle,
              TFT_BLACK, TFT_BLACK, false);
}

void clearTimerText(int16_t centerX, int16_t centerY) {
  tft.fillRect(centerX - 24, centerY - 16, 48, 32, TFT_BLACK);
}

void drawTimerSeconds(int16_t centerX, int16_t centerY, uint8_t secondsLeft) {
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  char timerText[4];
  snprintf(timerText, sizeof(timerText), "%u", secondsLeft);
  tft.drawString(timerText, centerX, centerY);
}

void drawCancelHint(int16_t centerX) {
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(BC_DATUM);
  tft.drawString("BTN3: change pickup", centerX,
                 tft.height() - CANCEL_HINT_BOTTOM_MARGIN);
}

bool detectDemoMode() {
  // Keep the logo visible while Wi-Fi is starting. A deliberate BTN2 hold
  // enters demo mode; a normal BTN2 press remains the order-button choice.
  uint32_t button2HeldSince = 0;
  while (true) {
    const Button button = readButton();
    if (button == Button::Button2) {
      if (button2HeldSince == 0) {
        button2HeldSince = millis();
      } else if (millis() - button2HeldSince >= DEMO_HOLD_MS) {
        waitForButtonRelease();
        return true;
      }
    } else {
      button2HeldSince = 0;
    }
    if (deviceWiFiConnected() && button == Button::None) {
      return false;
    }
    delay(10);
  }
}

void drawProcessingOrder() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Processing your order", tft.width() / 2,
                 tft.height() / 2);
}

void drawOrderSubmitFailed(const String& detail) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Order request failed", tft.width() / 2, 92);
  tft.setTextSize(1);
  String firstLine = detail;
  String secondLine;
  constexpr uint8_t MAX_LINE_LENGTH = 42;
  if (firstLine.length() > MAX_LINE_LENGTH) {
    int splitAt = firstLine.lastIndexOf(' ', MAX_LINE_LENGTH);
    if (splitAt < 1) {
      splitAt = MAX_LINE_LENGTH;
    }
    secondLine = firstLine.substring(splitAt + 1);
    firstLine = firstLine.substring(0, splitAt);
  }
  tft.drawString(firstLine.length() > 0 ? firstLine.c_str()
                                        : "Please try again",
                 tft.width() / 2, 128);
  if (secondLine.length() > 0) {
    tft.drawString(secondLine.substring(0, MAX_LINE_LENGTH).c_str(),
                   tft.width() / 2, 145);
  }
}

void drawThankYouBlock(TFT_eSPI& display, int16_t x, int16_t thankY,
                       int16_t youY) {
  display.setTextFont(1);
  display.setTextSize(5);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.drawString("Thank", x, thankY);
  display.drawString("You!", x, youY);
}

void animateThankYouScroll(int16_t fromX, int16_t toX, int16_t thankY,
                           int16_t youY, int16_t bandTop, int16_t bandHeight,
                           uint32_t durationMs) {
  TFT_eSprite sprite(&tft);
  sprite.createSprite(tft.width(), bandHeight);

  const uint32_t startMs = millis();
  while (millis() - startMs < durationMs) {
    const float progress =
        (millis() - startMs) / static_cast<float>(durationMs);
    const int16_t x = fromX + static_cast<int16_t>((toX - fromX) * progress);

    sprite.fillSprite(TFT_BLACK);
    drawThankYouBlock(sprite, x, thankY, youY);
    sprite.pushSprite(0, bandTop);
    delay(THANK_YOU_FRAME_MS);
  }

  sprite.fillSprite(TFT_BLACK);
  drawThankYouBlock(sprite, toX, thankY, youY);
  sprite.pushSprite(0, bandTop);
  sprite.deleteSprite();
}

void playThankYouAnimation(const String* orderId = nullptr,
                           OrderStatus* initialStatus = nullptr) {
  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();
  const int16_t centerX = screenW / 2;
  const int16_t centerY = screenH / 2;

  tft.setTextFont(1);
  tft.setTextSize(5);
  const int16_t lineHeight = tft.fontHeight();
  const int16_t thankY = centerY - lineHeight / 2 - 4;
  const int16_t youY = centerY + lineHeight / 2 + 4;
  const int16_t bandTop = thankY - lineHeight / 2 - 8;
  const int16_t bandHeight = youY - thankY + lineHeight + 16;
  const int16_t thankYInBand = thankY - bandTop;
  const int16_t youYInBand = youY - bandTop;
  const int16_t startX = screenW + 80;
  const int16_t endX = centerX;
  const int16_t exitX = -80;

  tft.fillScreen(TFT_BLACK);

  animateThankYouScroll(startX, endX, thankYInBand, youYInBand, bandTop,
                        bandHeight, THANK_YOU_SCROLL_IN_MS);
  const uint32_t holdStartedAt = millis();
  if (orderId != nullptr && initialStatus != nullptr) {
    while (!fetchOrderStatus(*orderId, *initialStatus)) {
      delay(1000);
    }
  }
  while (millis() - holdStartedAt < THANK_YOU_HOLD_MS) {
    delay(20);
  }
  animateThankYouScroll(endX, exitX, thankYInBand, youYInBand, bandTop,
                        bandHeight, THANK_YOU_SCROLL_OUT_MS);

  tft.fillRect(0, bandTop, screenW, bandHeight, TFT_BLACK);
}

bool gameButtonPressed() {
  // Any physical button opens Games from the status screen.
  return readButtonEvent() != Button::None;
}

Button waitForPowerButton() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_UNDEFINED) {
    digitalWrite(BACKLIGHT_PIN, LOW);
    esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_WAKE_PIN,
                                  ESP_EXT1_WAKEUP_ALL_LOW);
    esp_deep_sleep_start();
  }

  // Any ladder button wakes and starts the device.
  while (true) {
    const Button button = readButtonEvent();
    if (button != Button::None) {
      startDeviceWiFi();
      waitForButtonRelease();
      return button;
    }
    delay(10);
  }
}

void enterStandby(bool playShutdownSound) {
  if (playShutdownSound) {
    playPowerDownSound();
  }

  digitalWrite(BACKLIGHT_PIN, LOW);
  esp_sleep_enable_ext1_wakeup(1ULL << BUTTON_WAKE_PIN,
                                ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}

void drawGameButton(TFT_eSPI& display) {
  display.fillRoundRect(GAME_BUTTON_X, GAME_BUTTON_Y, GAME_BUTTON_W,
                        GAME_BUTTON_H, 6, TFT_BLACK);
  display.drawRoundRect(GAME_BUTTON_X, GAME_BUTTON_Y, GAME_BUTTON_W,
                        GAME_BUTTON_H, 6, TFT_WHITE);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.drawString("Game", GAME_BUTTON_X + GAME_BUTTON_W / 2,
                     GAME_BUTTON_Y + GAME_BUTTON_H / 2);
}

String notificationMessageText(const String& message) {
  constexpr uint8_t MAX_MESSAGE_CHARS = 40;
  if (message.length() <= MAX_MESSAGE_CHARS) {
    return message;
  }
  return message.substring(0, MAX_MESSAGE_CHARS - 3) + "...";
}

void appendMessageNotification(MessageNotificationState& state,
                               const DeviceMessage& message) {
  if (state.count > 0 &&
      static_cast<int32_t>(state.expiresAt - millis()) <= 0) {
    state.count = 0;
  }
  if (state.count == MAX_MESSAGE_NOTIFICATIONS) {
    for (uint8_t index = 1; index < state.count; ++index) {
      state.messages[index - 1] = state.messages[index];
    }
    --state.count;
  }
  state.messages[state.count++] = message;
  state.expiresAt = millis() + MESSAGE_NOTIFICATION_DURATION_MS;
}

bool messageNotificationActive(MessageNotificationState& state) {
  if (state.count == 0) {
    return false;
  }
  if (static_cast<int32_t>(state.expiresAt - millis()) <= 0) {
    state.count = 0;
    return false;
  }
  return true;
}

void messagePollWorkerTask(void* parameter) {
  auto* worker = static_cast<MessagePollWorker*>(parameter);
  uint32_t sinceMessageId = 0;
  uint32_t nextPollAt = 0;

  while (!worker->stopRequested) {
    if (!worker->paused && millis() >= nextPollAt) {
      DeviceMessage messages[MESSAGE_POLL_LIMIT];
      uint8_t messageCount = 0;
      uint32_t nextSinceMessageId = sinceMessageId;
      const DeviceMessagePollStatus result = fetchDeviceMessages(
          0, sinceMessageId, messages, MESSAGE_POLL_LIMIT, messageCount,
          nextSinceMessageId);
      if (result == DeviceMessagePollStatus::Success) {
        sinceMessageId = nextSinceMessageId;
        if (messageCount > 0 &&
            xSemaphoreTake(worker->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          for (uint8_t index = 0; index < messageCount; ++index) {
            worker->messages[index] = messages[index];
          }
          worker->messageCount = messageCount;
          worker->resultReady = true;
          xSemaphoreGive(worker->mutex);
        }
      }
      nextPollAt = millis() + 3000;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  worker->finished = true;
  vTaskDelete(nullptr);
}

bool startMessagePollWorker(MessagePollWorker& worker) {
  worker.mutex = xSemaphoreCreateMutex();
  if (worker.mutex == nullptr) {
    return false;
  }

  const BaseType_t created = xTaskCreatePinnedToCore(
      messagePollWorkerTask, "message_poll", 8192, &worker, 1,
      &worker.task, 0);
  if (created != pdPASS) {
    vSemaphoreDelete(worker.mutex);
    worker.mutex = nullptr;
    return false;
  }
  return true;
}

void stopMessagePollWorker(MessagePollWorker& worker) {
  if (worker.mutex == nullptr) {
    return;
  }

  worker.stopRequested = true;
  while (!worker.finished) {
    delay(1);
  }
  vSemaphoreDelete(worker.mutex);
  worker.mutex = nullptr;
}

void consumeMessagePollResults(MessagePollWorker& worker,
                               MessageNotificationState& state) {
  if (worker.mutex == nullptr ||
      xSemaphoreTake(worker.mutex, 0) != pdTRUE) {
    return;
  }

  DeviceMessage messages[MESSAGE_POLL_LIMIT];
  const uint8_t messageCount =
      worker.resultReady ? worker.messageCount : 0;
  for (uint8_t index = 0; index < messageCount; ++index) {
    messages[index] = worker.messages[index];
  }
  worker.resultReady = false;
  xSemaphoreGive(worker.mutex);

  for (uint8_t index = 0; index < messageCount; ++index) {
    appendMessageNotification(state, messages[index]);
    playMessageNotificationSound();
  }
}

void drawMessageNotificationOverlay(TFT_eSPI& display,
                                    const MessageNotificationState& state) {
  const int32_t remainingSigned =
      static_cast<int32_t>(state.expiresAt - millis());
  if (state.count == 0 || remainingSigned <= 0) {
    return;
  }

  const int16_t panelHeight =
      28 + state.count * 34;
  const int16_t panelX = (display.width() - MESSAGE_PANEL_WIDTH) / 2;
  const int16_t panelY = (display.height() - panelHeight) / 2;
  const int16_t bubbleX = panelX + 14;
  const int16_t bubbleWidth = MESSAGE_PANEL_WIDTH - 28;

  display.fillRoundRect(panelX, panelY, MESSAGE_PANEL_WIDTH, panelHeight, 10,
                        TFT_DARKGREY);
  display.drawRoundRect(panelX, panelY, MESSAGE_PANEL_WIDTH, panelHeight, 10,
                        TFT_WHITE);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextDatum(TL_DATUM);

  int16_t contentY = panelY + 7;
  for (uint8_t index = 0; index < state.count; ++index) {
    const DeviceMessage& message = state.messages[index];
    display.setTextColor(TFT_WHITE, TFT_DARKGREY);
    display.drawString(
        message.flyerName.length() > 0 ? message.flyerName.c_str() : "Flyer",
        bubbleX, contentY);
    contentY += 10;

    display.fillRoundRect(bubbleX, contentY, bubbleWidth, 20, 8, TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.drawString(notificationMessageText(message.message).c_str(),
                       bubbleX + 8, contentY + 6);
    contentY += 24;
  }

  const uint16_t barWidth = MESSAGE_PANEL_WIDTH - 16;
  const uint16_t remainingWidth = static_cast<uint16_t>(
      barWidth * static_cast<uint32_t>(remainingSigned) /
      MESSAGE_NOTIFICATION_DURATION_MS);
  display.fillRect(panelX + 8, panelY + panelHeight - 7, barWidth, 3,
                   TFT_DARKGREY);
  if (remainingWidth > 0) {
    display.fillRect(panelX + 8, panelY + panelHeight - 7, remainingWidth, 3,
                     TFT_WHITE);
  }
}

String formatOrderTime(const String& timestamp) {
  int timeSeparator = timestamp.indexOf('T');
  if (timeSeparator < 0) {
    timeSeparator = timestamp.indexOf(' ');
  }
  if (timeSeparator < 0 || timeSeparator + 5 >= timestamp.length()) {
    return timestamp;
  }

  const int hourStart = timeSeparator + 1;
  const int minuteStart = hourStart + 3;
  if (!isDigit(timestamp[hourStart]) || !isDigit(timestamp[hourStart + 1]) ||
      !isDigit(timestamp[minuteStart]) ||
      !isDigit(timestamp[minuteStart + 1])) {
    return timestamp;
  }

  const int hour = timestamp.substring(hourStart, hourStart + 2).toInt();
  const int minute =
      timestamp.substring(minuteStart, minuteStart + 2).toInt();
  if (hour > 23 || minute > 59) {
    return timestamp;
  }

  // The API timestamps are UTC (their trailing Z means UTC). Display them
  // in fixed Eastern Standard Time as requested.
  int easternHour = hour - 5;
  if (easternHour < 0) {
    easternHour += 24;
  }
  const int displayHour = easternHour % 12 == 0 ? 12 : easternHour % 12;
  char formattedTime[12];
  snprintf(formattedTime, sizeof(formattedTime), "%d:%02d %s", displayHour,
           minute, easternHour >= 12 ? "PM" : "AM");
  return String(formattedTime);
}

String flyerAssignedLabel(const OrderStatus& status) {
  if (status.flasherName.length() == 0) {
    return String("Flyer Assigned");
  }

  return status.flasherName + " Assigned";
}

uint8_t orderStatusStage(const OrderStatus& status) {
  String normalizedStatus = status.orderStatus;
  normalizedStatus.toLowerCase();
  if (status.deliveredAt.length() > 0 || normalizedStatus == "3" ||
      normalizedStatus.indexOf("deliver") >= 0) {
    return 3;
  }
  if (status.pickedUpAt.length() > 0 || normalizedStatus == "2" ||
      normalizedStatus.indexOf("pickup") >= 0) {
    return 2;
  }
  if (status.flasherAssigned || normalizedStatus == "1" ||
      normalizedStatus.indexOf("assign") >= 0) {
    return 1;
  }
  return 0;
}

String orderStageDescription(const OrderStatus& status, uint8_t stage) {
  switch (stage) {
    case 0:
      if (status.orderPlacedAt.length() > 0) {
        return String("Placed ") + formatOrderTime(status.orderPlacedAt);
      }
      if (status.customerOrderPlacedAt.length() > 0) {
        return String("Placed ") +
               formatOrderTime(status.customerOrderPlacedAt);
      }
      return String("Order placed");
    case 1:
      if (status.flasherName.length() > 0) {
        return flyerAssignedLabel(status);
      }
      return status.flasherAssigned ? String("Flyer Assigned")
                                    : String("Awaiting flasher");
    case 2:
      if (status.pickedUpAt.length() > 0 &&
          formatOrderTime(status.pickedUpAt) != status.pickedUpAt) {
        const String flyerName =
            status.flasherName.length() > 0 ? status.flasherName : "Flyer";
        return flyerName + " picked up order at " +
               formatOrderTime(status.pickedUpAt);
      }
      return status.flasherName.length() > 0
                 ? status.flasherName + " picked up order"
                 : String("Order picked up");
    default:
      if (status.deliveredAt.length() > 0) {
        return String("Delivered at ") + formatOrderTime(status.deliveredAt);
      }
      return String("Delivered");
  }
}

void drawOrderTimelineFrame(TFT_eSPI& display, const String& orderId,
                            const OrderStatus& status, bool hasStatus,
                            int16_t scrollOffset, uint8_t activeStage,
                            const MessageNotificationState* notifications =
                                nullptr) {
  const int16_t screenH = display.height();
  display.fillRect(0, 0, display.width(), display.height(), ORDER_BG);

  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, ORDER_BG);
  display.setTextDatum(TL_DATUM);
  const String displayOrderId =
      status.orderNumber.length() > 0 ? status.orderNumber : orderId;
  const String orderLabel = String("Order #") + displayOrderId;
  display.drawString(orderLabel.c_str(), 12, 7);

  const int16_t firstY = ORDER_TIMELINE_FIRST_Y - scrollOffset;
  const int16_t lastY =
      firstY + 3 * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
  int16_t lineTop = firstY;
  int16_t lineBottom = lastY;
  if (lineTop < ORDER_TIMELINE_VIEW_TOP) {
    lineTop = ORDER_TIMELINE_VIEW_TOP;
  }
  if (lineBottom >= screenH) {
    lineBottom = screenH - 1;
  }
  if (lineTop <= lineBottom) {
    display.drawFastVLine(ORDER_TIMELINE_NODE_X, lineTop,
                          lineBottom - lineTop + 1, ORDER_BLUE);
  }

  constexpr const char* titles[] = {
      "Order Placed", "Accepted", "Picked Up", "Delivered"};
  for (uint8_t index = 0; index < 4; ++index) {
    const int16_t nodeY =
        firstY + index * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
    if (nodeY < ORDER_TIMELINE_VIEW_TOP ||
        nodeY >= screenH + ORDER_TIMELINE_NODE_RADIUS) {
      continue;
    }

    const bool completed = hasStatus && index <= activeStage;
    display.fillCircle(ORDER_TIMELINE_NODE_X, nodeY, ORDER_TIMELINE_NODE_RADIUS,
                       completed ? ORDER_BLUE : ORDER_BG);
    display.drawCircle(ORDER_TIMELINE_NODE_X, nodeY,
                       ORDER_TIMELINE_NODE_RADIUS + 2, ORDER_BLUE);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(TFT_WHITE, ORDER_BG);
    display.drawString(titles[index], ORDER_TIMELINE_TEXT_X, nodeY - 19);

    if (completed) {
      const String description = orderStageDescription(status, index);
      display.setTextColor(ORDER_SUBTEXT, ORDER_BG);
      display.drawString(description.c_str(), ORDER_TIMELINE_TEXT_X, nodeY + 5);
    }
  }

  drawGameButton(display);
  if (notifications != nullptr) {
    drawMessageNotificationOverlay(display, *notifications);
  }
}

bool waitForPostDeliveryStandby() {
  const uint32_t startedAt = millis();

  while (true) {
    if (readButtonEvent() != Button::None) {
      waitForButtonRelease();
      return true;
    }

    const uint32_t elapsedMs = millis() - startedAt;
    if (elapsedMs >= POST_DELIVERY_STANDBY_MS) {
      return false;
    }

    delay(10);
  }
}

bool createTimelineSprite(TFT_eSprite& screen) {
  // An 8-bit sprite cuts the amount of data pushed per frame in half.
  screen.setColorDepth(8);
  screen.createSprite(tft.width(), tft.height());
  return screen.created();
}

void showGamesFromTimeline(TFT_eSprite& screen, bool& useSprite,
                           const String& orderId, const OrderStatus& status,
                           bool hasStatus, int16_t scrollOffset,
                           uint8_t activeStage,
                           const MessageNotificationState* notifications) {
  // The game needs the same large sprite buffer. Release the timeline buffer
  // first so the game does not silently fall back to direct, flickery drawing.
  screen.deleteSprite();
  useSprite = false;

  if (activeMessagePollWorker != nullptr) {
    activeMessagePollWorker->paused = true;
  }
  showGameMenu(tft);
  if (activeMessagePollWorker != nullptr) {
    activeMessagePollWorker->paused = false;
  }

  // Recreate the timeline buffer after the game has released its buffer.
  useSprite = createTimelineSprite(screen);
  if (useSprite) {
    drawOrderTimelineFrame(screen, orderId, status, hasStatus, scrollOffset,
                           activeStage, notifications);
    screen.pushSprite(0, 0);
  } else {
    drawOrderTimelineFrame(tft, orderId, status, hasStatus, scrollOffset,
                           activeStage, notifications);
  }
}

void presentOrderTimeline(TFT_eSprite& screen, bool useSprite,
                          const String& orderId, const OrderStatus& status,
                          bool hasStatus, int16_t scrollOffset,
                          uint8_t activeStage,
                          const MessageNotificationState* notifications) {
  if (useSprite) {
    drawOrderTimelineFrame(screen, orderId, status, hasStatus, scrollOffset,
                           activeStage, notifications);
    screen.pushSprite(0, 0);
  } else {
    drawOrderTimelineFrame(tft, orderId, status, hasStatus, scrollOffset,
                           activeStage, notifications);
  }
}

bool orderIsDelivered(const OrderStatus& status) {
  if (status.deliveredAt.length() > 0) {
    return true;
  }

  String normalizedStatus = status.orderStatus;
  normalizedStatus.toLowerCase();
  return normalizedStatus == "3" ||
         normalizedStatus.indexOf("deliver") >= 0;
}

bool orderIsCanceledOrRefunded(const OrderStatus& status) {
  if (status.canceledOrRefunded) {
    return true;
  }

  String normalizedStatus = status.orderStatus;
  normalizedStatus.toLowerCase();
  return normalizedStatus == "-1" ||
         normalizedStatus.indexOf("cancel") >= 0 ||
         normalizedStatus.indexOf("refund") >= 0;
}

void animateOrderTimeline(TFT_eSprite& screen, bool& useSprite,
                          const String& orderId, const OrderStatus& status,
                          bool hasStatus, int16_t& scrollOffset,
                          uint8_t startStage, uint8_t targetStage,
                          const MessageNotificationState* notifications) {
  for (uint8_t stage = startStage + 1; stage <= targetStage; ++stage) {
    const int16_t startOffset = scrollOffset;
    const int16_t nextOffset =
        stage * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
    const uint32_t scrollStart = millis();

    while (millis() - scrollStart < ORDER_STATUS_SCROLL_MS) {
      const float progress =
          (millis() - scrollStart) /
          static_cast<float>(ORDER_STATUS_SCROLL_MS);
      scrollOffset =
          startOffset +
          static_cast<int16_t>((nextOffset - startOffset) * progress);
      presentOrderTimeline(screen, useSprite, orderId, status, hasStatus,
                           scrollOffset, stage, notifications);
      if (gameButtonPressed()) {
        waitForButtonRelease();
        showGamesFromTimeline(screen, useSprite, orderId, status, hasStatus,
                              scrollOffset, stage, notifications);
      }
      delay(ORDER_STATUS_FRAME_MS);
    }

    scrollOffset = nextOffset;
    presentOrderTimeline(screen, useSprite, orderId, status, hasStatus,
                         scrollOffset, stage, notifications);
  }
}

void setDemoOrderStage(OrderStatus& status, uint8_t stage) {
  status.orderStatus = String(stage);
  if (stage >= 1) {
    status.flasherAssigned = true;
    status.flasherName = "Demo Flyer";
  }
  if (stage >= 3) {
    status.deliveredAt = "Demo delivered";
  }
}

void showOrderStatus(const String& orderId, bool demoMode,
                     const OrderStatus* initialStatus = nullptr) {
  TFT_eSprite screen(&tft);
  bool useSprite = createTimelineSprite(screen);

  OrderStatus status = {};
  MessageNotificationState notifications;
  bool hasStatus = initialStatus != nullptr;
  uint8_t activeStage = initialStatus == nullptr
                            ? 0
                            : orderStatusStage(*initialStatus);
  int16_t scrollOffset =
      activeStage * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
  uint32_t nextPollAt =
      initialStatus == nullptr ? 0 : millis() + ORDER_STATUS_POLL_MS;
  bool canceledOrRefunded =
      initialStatus != nullptr && orderIsCanceledOrRefunded(*initialStatus);
  uint32_t nextDemoStageAt = millis() + ORDER_STATUS_HOLD_MS;

  if (initialStatus != nullptr) {
    status = *initialStatus;
  }

  if (demoMode) {
    status.orderStatus = "0";
    status.orderPlaced = true;
    status.orderPlacedAt = "Demo order";
    status.customerOrderPlacedAt = "Demo order";
    hasStatus = true;
  }

  // Draw the prefetched status before any network polling so there is no
  // blank pause after the Thank You animation leaves the screen.
  presentOrderTimeline(screen, useSprite, orderId, status, hasStatus,
                       scrollOffset, activeStage, &notifications);

  MessagePollWorker messagePollWorker;
  const bool messagePollWorkerStarted =
      !demoMode && startMessagePollWorker(messagePollWorker);
  if (messagePollWorkerStarted) {
    activeMessagePollWorker = &messagePollWorker;
  }

  while (true) {
    const uint32_t now = millis();
    if (messagePollWorkerStarted) {
      consumeMessagePollResults(messagePollWorker, notifications);
    }
    const bool notificationActive =
        messageNotificationActive(notifications);
    if (demoMode && now >= nextDemoStageAt) {
      if (activeStage < 3) {
        const uint8_t nextStage = activeStage + 1;
        setDemoOrderStage(status, nextStage);
        animateOrderTimeline(screen, useSprite, orderId, status, true,
                              scrollOffset, activeStage, nextStage,
                              &notifications);
        activeStage = nextStage;
        nextDemoStageAt = millis() + ORDER_STATUS_HOLD_MS;
      } else {
        break;
      }
    }

    if (!demoMode && !notificationActive && now >= nextPollAt) {
      OrderStatus latestStatus = {};
      if (fetchOrderStatus(orderId, latestStatus)) {
        const uint8_t targetStage = orderStatusStage(latestStatus);
        status = latestStatus;
        hasStatus = true;
        if (orderIsCanceledOrRefunded(status)) {
          canceledOrRefunded = true;
        }
        if (targetStage > activeStage) {
          animateOrderTimeline(screen, useSprite, orderId, status, true,
                                scrollOffset, activeStage, targetStage,
                                &notifications);
          activeStage = targetStage;
        }
      }
      nextPollAt = millis() + ORDER_STATUS_POLL_MS;
    }

    presentOrderTimeline(screen, useSprite, orderId, status, hasStatus,
                         scrollOffset, activeStage, &notifications);

    if (gameButtonPressed()) {
      waitForButtonRelease();
      showGamesFromTimeline(screen, useSprite, orderId, status, hasStatus,
                            scrollOffset, activeStage, &notifications);
    }

    if (!demoMode && hasStatus &&
        (orderIsDelivered(status) || canceledOrRefunded)) {
      break;
    }

    delay(ORDER_STATUS_FRAME_MS);
  }

  if (messagePollWorkerStarted) {
    activeMessagePollWorker->paused = true;
    stopMessagePollWorker(messagePollWorker);
    activeMessagePollWorker = nullptr;
  }

  if (canceledOrRefunded) {
    screen.deleteSprite();
    enterStandby(false);
    return;
  }

  for (uint8_t repeat = 0; repeat < DELIVERY_SOUND_REPEATS; ++repeat) {
    playDeliverySound();
    if (repeat + 1 < DELIVERY_SOUND_REPEATS) {
      delay(DELIVERY_SOUND_GAP_MS);
    }
  }

  screen.deleteSprite();
  const bool cancelled = waitForPostDeliveryStandby();
  enterStandby(cancelled);
}

void drawPickupLocationMenu(TFT_eSPI& display, uint8_t selectedMask,
                            uint8_t activeRow, uint8_t activeColumn) {
  display.fillScreen(TFT_BLACK);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(TC_DATUM);
  display.drawString("Pickup locations", display.width() / 2, 10);

  display.setTextSize(1);
  for (uint8_t index = 0; index < PICKUP_LOCATION_COUNT; ++index) {
    const int16_t column = index % 2;
    const int16_t row = index / 2;
    const int16_t x =
        PICKUP_MENU_LEFT + column * (PICKUP_MENU_TILE_WIDTH + PICKUP_MENU_GAP);
    const int16_t y =
        PICKUP_MENU_TOP + row * (PICKUP_MENU_TILE_HEIGHT + PICKUP_MENU_GAP);
    const bool selected = (selectedMask & (1U << index)) != 0;

    display.fillRoundRect(x, y, PICKUP_MENU_TILE_WIDTH, PICKUP_MENU_TILE_HEIGHT,
                          7, selected ? TFT_DARKGREY : TFT_BLACK);
    display.drawRoundRect(x, y, PICKUP_MENU_TILE_WIDTH, PICKUP_MENU_TILE_HEIGHT,
                          7, TFT_WHITE);

    display.setTextColor(TFT_WHITE, selected ? TFT_DARKGREY : TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    const PickupLocationDisplay& location = PICKUP_LOCATIONS[index];
    if (location.lineTwo[0] == '\0') {
      display.drawString(location.shortName, x + PICKUP_MENU_TILE_WIDTH / 2,
                         y + 14);
      display.drawString(location.lineOne, x + PICKUP_MENU_TILE_WIDTH / 2,
                         y + 33);
    } else {
      display.drawString(location.shortName, x + PICKUP_MENU_TILE_WIDTH / 2,
                         y + 9);
      display.drawString(location.lineOne, x + PICKUP_MENU_TILE_WIDTH / 2,
                         y + 24);
      display.drawString(location.lineTwo, x + PICKUP_MENU_TILE_WIDTH / 2,
                         y + 38);
    }
  }

  const int16_t activeX =
      PICKUP_MENU_LEFT + activeColumn * (PICKUP_MENU_TILE_WIDTH + PICKUP_MENU_GAP);
  const int16_t activeY =
      PICKUP_MENU_TOP + activeRow * (PICKUP_MENU_TILE_HEIGHT + PICKUP_MENU_GAP);
  display.drawRoundRect(activeX + 2, activeY + 2, PICKUP_MENU_TILE_WIDTH - 4,
                        PICKUP_MENU_TILE_HEIGHT - 4, 5, TFT_LIGHTGREY);

  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(BC_DATUM);
  display.drawString("BTN1 row   BTN2 column   BTN3 select   BTN4 done",
                     display.width() / 2, display.height() - 4);
}

uint8_t firstPickupLocationIndex(uint8_t mask) {
  for (uint8_t index = 0; index < PICKUP_LOCATION_COUNT; ++index) {
    if ((mask & (1U << index)) != 0) {
      return index;
    }
  }
  return 0;
}

uint8_t selectPickupLocations(uint8_t initialMask) {
  uint8_t selectedMask = initialMask;
  const uint8_t firstIndex = firstPickupLocationIndex(initialMask);
  uint8_t activeRow = firstIndex / 2;
  uint8_t activeColumn = firstIndex % 2;
  TFT_eSprite menuFrame(&tft);
  menuFrame.setColorDepth(8);
  const bool useSprite = menuFrame.createSprite(tft.width(), tft.height());
  bool needsRedraw = true;

  while (true) {
    if (needsRedraw) {
      if (useSprite) {
        drawPickupLocationMenu(menuFrame, selectedMask, activeRow,
                               activeColumn);
        menuFrame.pushSprite(0, 0);
      } else {
        drawPickupLocationMenu(tft, selectedMask, activeRow, activeColumn);
      }
      needsRedraw = false;
    }

    const Button button = readButtonEvent();
    if (button == Button::Button1) {
      const uint8_t rowCount = activeColumn == 0 ? 3 : 2;
      activeRow = (activeRow + 1) % rowCount;
      needsRedraw = true;
    } else if (button == Button::Button2) {
      activeColumn = (activeColumn + 1) % 2;
      activeRow = 0;
      needsRedraw = true;
    } else if (button == Button::Button3) {
      const uint8_t activeIndex = activeRow * 2 + activeColumn;
      selectedMask ^= static_cast<uint8_t>(1U << activeIndex);
      needsRedraw = true;
    } else if (button == Button::Button4) {
      waitForButtonRelease();
      menuFrame.deleteSprite();
      return selectedMask;
    }
    delay(10);
  }
}

void runCountdownTimer(const char* priceText, bool demoMode,
                       uint8_t orderButtonNumber,
                       uint8_t savedPickupLocationMask) {
  const int16_t centerX = tft.width() / 2;
  const int16_t centerY = TIMER_CENTER_Y;
  uint32_t startMs = millis();
  uint8_t pickupLocationMask = savedPickupLocationMask;

  drawPriceHeader(centerX, priceText, pickupLocationMask,
                  savedPickupLocationMask);
  drawFullTimerRing(centerX, centerY);
  drawTimerSeconds(centerX, centerY, 15);
  drawCancelHint(centerX);
  startGameAssetPrefetch();
  startGameAssetMemoryPreload();

  float erasedDegrees = 0.0f;
  uint8_t lastSecondsLeft = 15;

  while (true) {
    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs >= COUNTDOWN_DURATION_MS) {
      break;
    }
    const Button button = readButtonEvent();
    if (button == Button::Button3) {
      waitForButtonRelease();
      const uint32_t menuStartedAt = millis();
      pickupLocationMask = selectPickupLocations(pickupLocationMask);
      startMs += millis() - menuStartedAt;

      drawPriceHeader(centerX, priceText, pickupLocationMask,
                      savedPickupLocationMask);
      drawFullTimerRing(centerX, centerY);
      const uint32_t currentElapsedMs = millis() - startMs;
      const float currentDegrees =
          360.0f *
          (currentElapsedMs / static_cast<float>(COUNTDOWN_DURATION_MS));
      eraseTimerArcSegment(centerX, centerY, 0.0f,
                           min(currentDegrees, 360.0f));
      erasedDegrees = min(currentDegrees, 360.0f);
      clearTimerText(centerX, centerY);
      const uint32_t currentRemainingMs =
          COUNTDOWN_DURATION_MS > currentElapsedMs
              ? COUNTDOWN_DURATION_MS - currentElapsedMs
              : 0;
      lastSecondsLeft =
          static_cast<uint8_t>((currentRemainingMs + 999) / 1000);
      drawTimerSeconds(centerX, centerY, lastSecondsLeft);
      drawCancelHint(centerX);
      continue;
    }
    if (button != Button::None) {
      waitForButtonRelease();
      enterStandby(true);
    }

    const uint32_t remainingMs = COUNTDOWN_DURATION_MS - elapsedMs;
    const float newErasedDegrees =
        360.0f * (1.0f - (remainingMs / static_cast<float>(COUNTDOWN_DURATION_MS)));
    const uint8_t secondsLeft =
        static_cast<uint8_t>((remainingMs + 999) / 1000);

    if (newErasedDegrees > erasedDegrees) {
      eraseTimerArcSegment(centerX, centerY, erasedDegrees, newErasedDegrees);
      erasedDegrees = newErasedDegrees;
    }

    if (secondsLeft != lastSecondsLeft) {
      clearTimerText(centerX, centerY);
      drawTimerSeconds(centerX, centerY, secondsLeft);
      lastSecondsLeft = secondsLeft;
    }

    delay(33);
  }

  eraseTimerArcSegment(centerX, centerY, erasedDegrees, 360.0f);
  if (!demoMode) {
    String orderId;
    String eventId;
    // Always send the currently displayed pickup locations when available.
    // Omitting the field for an unchanged default causes some saved-location
    // orders to be rejected, while sending the same IDs is a valid override.
    const uint8_t pickupOverrideMask = pickupLocationMask;
    drawProcessingOrder();
    prepareGameAssetsForOrderRequest();
    DeviceOrderStatus orderStatus = DeviceOrderStatus::Unavailable;
    String orderFailureDetail;
    for (uint8_t attempt = 0; attempt < ORDER_SUBMIT_RETRIES; ++attempt) {
      orderStatus = submitDeviceOrder(orderButtonNumber, pickupOverrideMask,
                                      eventId, orderId, orderFailureDetail);
      if (orderStatus == DeviceOrderStatus::Success ||
          orderStatus == DeviceOrderStatus::Unarmed) {
        break;
      }
      if (orderStatus == DeviceOrderStatus::NeedsPairing) {
        ensureDevicePaired(tft);
        drawProcessingOrder();
      }
      delay(1000);
    }
    if (orderStatus != DeviceOrderStatus::Success) {
      if (orderStatus == DeviceOrderStatus::Unarmed) {
        drawButtonUnarmed(orderButtonNumber);
      } else {
        drawOrderSubmitFailed(orderFailureDetail);
      }
      delay(2000);
      enterStandby(false);
      return;
    }
    startGameAssetMemoryPreload();
    OrderStatus initialOrderStatus = {};
    playThankYouAnimation(&orderId, &initialOrderStatus);
    showOrderStatus(orderId, false, &initialOrderStatus);
    return;
  }

  playThankYouAnimation();
  showOrderStatus("DEMO", true);
}
}  // namespace

void setup() {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, LOW);

  initDeviceConfig();
  initButtons();
  const uint8_t otaResumeButton = takeOtaResumeButton();
  const Button wakeButton =
      otaResumeButton == 0 ? waitForPowerButton()
                           : static_cast<Button>(otaResumeButton);
  if (otaResumeButton != 0) {
    // A software restart has no deep-sleep wake cause. Resume the original
    // order button and reconnect instead of immediately sleeping again.
    startDeviceWiFi();
  }

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);

  drawSplashScreen();
  digitalWrite(BACKLIGHT_PIN, HIGH);
  playBootSound();
  const bool demoMode = detectDemoMode();
  if (deviceConfig().gameModeOnly) {
    checkForFirmwareUpdate(tft, static_cast<uint8_t>(wakeButton));
    showGameMenu(tft, true);
    enterStandby(true);
    return;
  }

  if (demoMode) {
    checkForFirmwareUpdate(tft, static_cast<uint8_t>(wakeButton));
    runCountdownTimer("DEMO $4.25", true, 0, 0);
    return;
  }

  ensureDevicePaired(tft);
  checkForFirmwareUpdate(tft, static_cast<uint8_t>(wakeButton));
  String priceText;
  uint8_t savedPickupLocationMask = 0;
  const uint8_t orderButtonNumber = static_cast<uint8_t>(wakeButton);
  DevicePriceStatus priceStatus =
      fetchPriceWithRetry(orderButtonNumber, priceText,
                          savedPickupLocationMask);
  if (priceStatus == DevicePriceStatus::NeedsPairing) {
    // The server rejected the saved token, so pair this device again.
    ensureDevicePaired(tft);
    priceStatus = fetchPriceWithRetry(orderButtonNumber, priceText,
                                      savedPickupLocationMask);
  }
  if (priceStatus != DevicePriceStatus::Success) {
    if (priceStatus == DevicePriceStatus::Unarmed) {
      drawButtonUnarmed(orderButtonNumber);
    } else {
      drawPriceUnavailable();
    }
    delay(2000);
    enterStandby(false);
  }
  runCountdownTimer(priceText.c_str(), false, orderButtonNumber,
                    savedPickupLocationMask);
}

void loop() {
  delay(1000);
}

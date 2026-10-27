#include <Arduino.h>
#include <TFT_eSPI.h>
#include <esp_sleep.h>

#include <cstdio>

#include "boot_sound.h"
#include "button_input.h"
#include "delivery_sound.h"
#include "device_network.h"
#include "display_config.h"
#include "falcon_logo.h"
#include "game_menu.h"
#include "power_down_sound.h"

#ifndef FALCON_GAME_MODE_ONLY
#define FALCON_GAME_MODE_ONLY 0
#endif

namespace {
constexpr uint8_t BACKLIGHT_PIN = 21;
constexpr uint8_t BUTTON_WAKE_PIN = 35;
constexpr bool GAME_MODE_ONLY = FALCON_GAME_MODE_ONLY != 0;
constexpr int16_t LOGO_TEXT_GAP = 4;
constexpr uint32_t SPLASH_DURATION_MS = 2000;
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

void drawPriceHeader(int16_t centerX, const char* priceText) {
  tft.fillScreen(TFT_BLACK);

  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(priceText, centerX, 20);
}

void drawPriceUnavailable() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Price unavailable", tft.width() / 2, 92);
  tft.setTextSize(1);
  tft.drawString("Device is not armed", tft.width() / 2, 132);
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
  tft.drawString("Press any button to cancel", centerX,
                 tft.height() - CANCEL_HINT_BOTTOM_MARGIN);
}

bool detectDemoMode() {
  const uint32_t startedAt = millis();
  while (millis() - startedAt < SPLASH_DURATION_MS) {
    if (readButtonEvent() == Button::Button2) {
      waitForButtonRelease();
      return true;
    }
    delay(10);
  }
  return false;
}

void drawOrderFailure() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextFont(1);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Order failed", tft.width() / 2, 92);
  tft.setTextSize(1);
  tft.drawString("Retrying selected order...", tft.width() / 2, 132);
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

void playThankYouAnimation() {
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
  delay(THANK_YOU_HOLD_MS);
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

void drawOrderTimelineFrame(TFT_eSPI& display, int16_t scrollOffset,
                            uint8_t activeStage) {
  constexpr const char* titles[] = {
      "Order Placed", "Accepted", "Picked Up", "Delivered"};
  constexpr const char* descriptions[] = {
      "9/14/2026, 7:29:00 PM",
      "Accepted by Emory H.",
      "Emory H. picked up the order",
      "Delivered by Emory H."};

  const int16_t screenH = display.height();
  display.fillRect(0, 0, display.width(), display.height(), ORDER_BG);

  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, ORDER_BG);
  display.setTextDatum(TL_DATUM);
  display.drawString("Order #123456789", 12, 7);

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

  for (uint8_t index = 0; index < 4; ++index) {
    const int16_t nodeY =
        firstY + index * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
    if (nodeY < ORDER_TIMELINE_VIEW_TOP ||
        nodeY >= screenH + ORDER_TIMELINE_NODE_RADIUS) {
      continue;
    }

    const bool completed = index <= activeStage;
    display.fillCircle(ORDER_TIMELINE_NODE_X, nodeY, ORDER_TIMELINE_NODE_RADIUS,
                       completed ? ORDER_BLUE : ORDER_BG);
    display.drawCircle(ORDER_TIMELINE_NODE_X, nodeY,
                       ORDER_TIMELINE_NODE_RADIUS + 2, ORDER_BLUE);

    display.setTextFont(1);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE, ORDER_BG);
    display.setTextDatum(TL_DATUM);
    display.drawString(titles[index], ORDER_TIMELINE_TEXT_X, nodeY - 19);

    if (completed) {
      display.setTextFont(1);
      display.setTextSize(1);
      display.setTextColor(ORDER_SUBTEXT, ORDER_BG);
      display.drawString(descriptions[index], ORDER_TIMELINE_TEXT_X, nodeY + 5);
    }
  }

  drawGameButton(display);
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
                           int16_t scrollOffset, uint8_t activeStage) {
  // The game needs the same large sprite buffer. Release the timeline buffer
  // first so the game does not silently fall back to direct, flickery drawing.
  screen.deleteSprite();
  useSprite = false;

  showGameMenu(tft);

  // Recreate the timeline buffer after the game has released its buffer.
  useSprite = createTimelineSprite(screen);
  if (useSprite) {
    drawOrderTimelineFrame(screen, scrollOffset, activeStage);
    screen.pushSprite(0, 0);
  } else {
    drawOrderTimelineFrame(tft, scrollOffset, activeStage);
  }
}

void showOrderTimeline() {
  TFT_eSprite screen(&tft);
  bool useSprite = createTimelineSprite(screen);

  int16_t scrollOffset = 0;
  if (useSprite) {
    drawOrderTimelineFrame(screen, scrollOffset, 0);
    screen.pushSprite(0, 0);
  } else {
    drawOrderTimelineFrame(tft, scrollOffset, 0);
  }

  for (uint8_t stage = 1; stage < 4; ++stage) {
    const uint32_t holdStart = millis();
    while (millis() - holdStart < ORDER_STATUS_HOLD_MS) {
      if (gameButtonPressed()) {
        waitForButtonRelease();
        showGamesFromTimeline(screen, useSprite, scrollOffset, stage - 1);
      }
      delay(10);
    }

    const int16_t nextOffset =
        stage * static_cast<int16_t>(ORDER_TIMELINE_ROW_HEIGHT);
    const int16_t startOffset = scrollOffset;
    const uint32_t scrollStart = millis();
    while (millis() - scrollStart < ORDER_STATUS_SCROLL_MS) {
      const float progress =
          (millis() - scrollStart) /
          static_cast<float>(ORDER_STATUS_SCROLL_MS);
      scrollOffset =
          startOffset +
          static_cast<int16_t>((nextOffset - startOffset) * progress);
      if (useSprite) {
        drawOrderTimelineFrame(screen, scrollOffset, stage);
        screen.pushSprite(0, 0);
      } else {
        drawOrderTimelineFrame(tft, scrollOffset, stage);
      }
      if (gameButtonPressed()) {
        waitForButtonRelease();
        showGamesFromTimeline(screen, useSprite, scrollOffset, stage);
      }
      delay(ORDER_STATUS_FRAME_MS);
    }

    scrollOffset = nextOffset;
    if (useSprite) {
      drawOrderTimelineFrame(screen, scrollOffset, stage);
      screen.pushSprite(0, 0);
    } else {
      drawOrderTimelineFrame(tft, scrollOffset, stage);
    }
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

void runCountdownTimer(const char* priceText, bool demoMode,
                       uint8_t orderButtonNumber) {
  const int16_t centerX = tft.width() / 2;
  const int16_t centerY = TIMER_CENTER_Y;
  const uint32_t startMs = millis();

  drawPriceHeader(centerX, priceText);
  drawFullTimerRing(centerX, centerY);
  drawTimerSeconds(centerX, centerY, 15);
  drawCancelHint(centerX);

  float erasedDegrees = 0.0f;
  uint8_t lastSecondsLeft = 15;

  while (true) {
    const uint32_t elapsedMs = millis() - startMs;
    if (elapsedMs >= COUNTDOWN_DURATION_MS) {
      break;
    }
    if (readButtonEvent() != Button::None) {
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
    while (!submitDeviceOrder(orderButtonNumber)) {
      drawOrderFailure();
      delay(1000);
    }
  }

  playThankYouAnimation();
  showOrderTimeline();
}
}  // namespace

void setup() {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, LOW);

  initButtons();
  const Button wakeButton = waitForPowerButton();

  tft.init();
  tft.setRotation(DISPLAY_ROTATION);

  drawSplashScreen();
  digitalWrite(BACKLIGHT_PIN, HIGH);
  playBootSound();
  const bool demoMode = detectDemoMode();
  if (GAME_MODE_ONLY) {
    showGameMenu(tft, true);
    enterStandby(true);
    return;
  }

  if (demoMode) {
    runCountdownTimer("DEMO $4.25", true, 0);
    return;
  }

  ensureDevicePaired(tft);
  String priceText;
  const uint8_t orderButtonNumber = static_cast<uint8_t>(wakeButton);
  DevicePriceStatus priceStatus =
      fetchDevicePrice(orderButtonNumber, priceText);
  if (priceStatus == DevicePriceStatus::NeedsPairing) {
    // The server rejected the saved token, so pair this device again.
    ensureDevicePaired(tft);
    priceStatus = fetchDevicePrice(orderButtonNumber, priceText);
  }
  if (priceStatus != DevicePriceStatus::Success) {
    drawPriceUnavailable();
    delay(2000);
    enterStandby(false);
  }
  runCountdownTimer(priceText.c_str(), false, orderButtonNumber);
}

void loop() {
  delay(1000);
}

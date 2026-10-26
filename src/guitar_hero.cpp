#include "guitar_hero.h"

#include <Arduino.h>

#include <cstdio>

#include "audio_helper.h"
#include "button_input.h"

namespace {
constexpr int16_t LANE_WIDTH = 96;
constexpr int16_t LANE_TOP = 34;
constexpr int16_t HIT_LINE_Y = 180;
constexpr int16_t NOTE_HEIGHT = 16;
constexpr int16_t HIT_WINDOW = 24;
constexpr float NOTE_SPEED = 0.24f;
constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t FIRST_NOTE_DELAY_MS = 600;
constexpr uint32_t NOTE_INTERVAL_MS = 850;
constexpr uint8_t MAX_MISSES = 5;

void playHitSound() {
  audioPlayTone(1175, 35);
}

void playMissSound() {
  audioPlayTone(220, 80);
}

void playGameOverSound() {
  audioPlayTone(392, 100);
  delay(20);
  audioPlayTone(262, 180);
}

void drawFrame(TFT_eSPI& display, float noteY, bool noteActive,
               uint16_t score, uint8_t combo, uint8_t misses) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();
  const int16_t laneLeft = (screenW - LANE_WIDTH) / 2;

  display.fillScreen(TFT_BLACK);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(TC_DATUM);
  display.drawString("Guitar Hero", screenW / 2, 6);

  display.setTextSize(1);
  display.setTextDatum(TL_DATUM);
  char scoreText[24];
  snprintf(scoreText, sizeof(scoreText), "Score: %u", score);
  display.drawString(scoreText, 8, 10);

  display.setTextDatum(TR_DATUM);
  char statusText[24];
  snprintf(statusText, sizeof(statusText), "Combo: %u  Miss: %u", combo,
           misses);
  display.drawString(statusText, screenW - 8, 10);

  display.fillRoundRect(laneLeft, LANE_TOP, LANE_WIDTH,
                        HIT_LINE_Y - LANE_TOP + 34, 8, TFT_DARKGREY);
  display.drawRoundRect(laneLeft, LANE_TOP, LANE_WIDTH,
                        HIT_LINE_Y - LANE_TOP + 34, 8, TFT_LIGHTGREY);

  display.drawFastHLine(laneLeft + 6, HIT_LINE_Y - 2, LANE_WIDTH - 12,
                        TFT_CYAN);
  display.drawFastHLine(laneLeft + 6, HIT_LINE_Y, LANE_WIDTH - 12,
                        TFT_WHITE);
  display.drawFastHLine(laneLeft + 6, HIT_LINE_Y + 2, LANE_WIDTH - 12,
                        TFT_CYAN);

  if (noteActive) {
    const int16_t noteTop =
        static_cast<int16_t>(noteY) - NOTE_HEIGHT / 2;
    display.fillRoundRect(laneLeft + 22, noteTop, LANE_WIDTH - 44,
                          NOTE_HEIGHT, 5, TFT_YELLOW);
    display.drawRoundRect(laneLeft + 22, noteTop, LANE_WIDTH - 44,
                          NOTE_HEIGHT, 5, TFT_WHITE);
  }

  display.setTextDatum(BC_DATUM);
  display.setTextColor(TFT_YELLOW, TFT_BLACK);
  display.drawString("BTN1 STRUM", screenW / 2, screenH - 18);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.drawString("BTN4 BACK", screenW / 2, screenH - 5);
}

void drawGameOver(TFT_eSPI& display, uint16_t score, uint8_t misses) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();

  display.fillScreen(TFT_BLACK);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.setTextFont(1);
  display.setTextSize(3);
  display.drawString("Game Over", screenW / 2, screenH / 2 - 28);

  display.setTextSize(1);
  char resultText[32];
  snprintf(resultText, sizeof(resultText), "Score: %u   Misses: %u", score,
           misses);
  display.drawString(resultText, screenW / 2, screenH / 2 + 10);
  display.drawString("BTN3 replay   BTN4 back", screenW / 2,
                     screenH / 2 + 34);
}
}  // namespace

void runGuitarHero(TFT_eSPI& display) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();

  waitForButtonRelease();
  // Keep the speaker enabled so short hit and miss sounds are audible.
  audioBegin();

  TFT_eSprite gameFrame(&display);
  gameFrame.setColorDepth(8);
  gameFrame.createSprite(screenW, screenH);
  const bool useSprite = gameFrame.created();

  while (true) {
    uint16_t score = 0;
    uint8_t combo = 0;
    uint8_t misses = 0;
    bool noteActive = false;
    float noteY = 0.0f;
    uint32_t nextNoteAt = millis() + FIRST_NOTE_DELAY_MS;
    uint32_t lastFrame = millis();
    bool gameOver = false;

    while (!gameOver) {
      const uint32_t now = millis();
      if (now - lastFrame < FRAME_MS) {
        delay(2);
        continue;
      }
      const uint32_t elapsedMs = now - lastFrame;
      lastFrame = now;

      const Button button = readButtonEvent();
      if (button == Button::Button4) {
        audioEnd();
        waitForButtonRelease();
        return;
      }

      if (!noteActive && now >= nextNoteAt) {
        noteActive = true;
        noteY = static_cast<float>(LANE_TOP + NOTE_HEIGHT / 2);
      }
      if (noteActive) {
        noteY += NOTE_SPEED * elapsedMs;
      }

      if (button == Button::Button1) {
        if (noteActive) {
          const float distance =
              noteY > HIT_LINE_Y ? noteY - HIT_LINE_Y : HIT_LINE_Y - noteY;
          if (distance <= HIT_WINDOW) {
            ++combo;
            score += static_cast<uint16_t>(100 + combo * 10);
            noteActive = false;
            nextNoteAt = now + NOTE_INTERVAL_MS;
            playHitSound();
          } else {
            combo = 0;
            playMissSound();
          }
        } else {
          combo = 0;
          playMissSound();
        }
      }

      if (noteActive && noteY > HIT_LINE_Y + HIT_WINDOW) {
        noteActive = false;
        combo = 0;
        ++misses;
        nextNoteAt = now + NOTE_INTERVAL_MS;
        playMissSound();
        if (misses >= MAX_MISSES) {
          gameOver = true;
        }
      }

      if (useSprite) {
        drawFrame(gameFrame, noteY, noteActive, score, combo, misses);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, noteY, noteActive, score, combo, misses);
      }
    }

    if (useSprite) {
      drawGameOver(gameFrame, score, misses);
      gameFrame.pushSprite(0, 0);
    } else {
      drawGameOver(display, score, misses);
    }
    playGameOverSound();

    while (true) {
      const Button button = readButtonEvent();
      if (button == Button::Button4) {
        audioEnd();
        waitForButtonRelease();
        return;
      }
      if (button == Button::Button3) {
        waitForButtonRelease();
        break;
      }
      delay(20);
    }
  }
}

#include "guitar_hero.h"

#include <Arduino.h>

#include <cstdio>

#include "audio_helper.h"
#include "button_input.h"

namespace {
constexpr uint8_t LANE_COUNT = 4;
constexpr int16_t LANE_WIDTH = 52;
constexpr int16_t LANE_GAP = 4;
constexpr int16_t TOTAL_LANE_WIDTH =
    LANE_COUNT * LANE_WIDTH + (LANE_COUNT - 1) * LANE_GAP;
constexpr int16_t LANE_TOP = 34;
constexpr int16_t HIT_LINE_Y = 180;
constexpr int16_t NOTE_HEIGHT = 16;
constexpr int16_t HIT_WINDOW = 24;
constexpr float NOTE_SPEED = 0.24f;
constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t FIRST_NOTE_DELAY_MS = 600;
constexpr uint32_t NOTE_INTERVAL_MS = 650;
constexpr uint8_t MAX_CONSECUTIVE_MISSES = 5;
constexpr uint32_t BUTTON4_BACK_HOLD_MS = 1500;

struct MelodyNote {
  uint16_t frequency;
  uint8_t lane;
};

constexpr MelodyNote MARY_MELODY[] = {
    {330, 2}, {294, 1}, {262, 0}, {294, 1}, {330, 2}, {330, 2}, {330, 2},
    {294, 1}, {294, 1}, {294, 1}, {330, 2}, {392, 3}, {392, 3}, {330, 2},
    {294, 1}, {262, 0}, {294, 1}, {330, 2}, {330, 2}, {330, 2}, {330, 2},
    {294, 1}, {294, 1}, {330, 2}, {294, 1}, {262, 0}};
constexpr uint8_t MARY_MELODY_LENGTH =
    sizeof(MARY_MELODY) / sizeof(MARY_MELODY[0]);

int8_t buttonLane(Button button) {
  switch (button) {
    case Button::Button1:
      return 0;
    case Button::Button2:
      return 1;
    case Button::Button3:
      return 2;
    case Button::Button4:
      return 3;
    default:
      return -1;
  }
}

uint16_t laneColor(uint8_t lane) {
  switch (lane) {
    case 0:
      return TFT_RED;
    case 1:
      return TFT_YELLOW;
    case 2:
      return TFT_GREEN;
    default:
      return TFT_BLUE;
  }
}

void playMelodyNote(uint16_t frequency) {
  audioPlayTone(frequency, 110);
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
               uint8_t noteLane, uint16_t score, uint8_t combo,
               uint8_t misses) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();
  const int16_t laneLeft = (screenW - TOTAL_LANE_WIDTH) / 2;

  display.fillScreen(TFT_BLACK);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(TC_DATUM);
  display.drawString("Mary Had a Little Lamb", screenW / 2, 6);
  display.drawString("Hold BTN4 = back", screenW / 2, 22);

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

  for (uint8_t lane = 0; lane < LANE_COUNT; ++lane) {
    const int16_t laneX = laneLeft + lane * (LANE_WIDTH + LANE_GAP);
    const uint16_t color = laneColor(lane);
    display.fillRoundRect(laneX, LANE_TOP, LANE_WIDTH,
                          HIT_LINE_Y - LANE_TOP + 34, 6, TFT_DARKGREY);
    display.drawRoundRect(laneX, LANE_TOP, LANE_WIDTH,
                          HIT_LINE_Y - LANE_TOP + 34, 6, color);
    display.fillRect(laneX + 4, HIT_LINE_Y - 6, LANE_WIDTH - 8, 12, color);
    display.drawRect(laneX + 4, HIT_LINE_Y - 6, LANE_WIDTH - 8, 12,
                     TFT_WHITE);

    display.setTextDatum(BC_DATUM);
    char buttonLabel[4];
    snprintf(buttonLabel, sizeof(buttonLabel), "B%u", lane + 1);
    display.drawString(buttonLabel, laneX + LANE_WIDTH / 2, screenH - 4);
  }

  if (noteActive && noteLane < LANE_COUNT) {
    const int16_t laneX = laneLeft + noteLane * (LANE_WIDTH + LANE_GAP);
    const int16_t noteTop =
        static_cast<int16_t>(noteY) - NOTE_HEIGHT / 2;
    display.fillRoundRect(laneX + 8, noteTop, LANE_WIDTH - 16, NOTE_HEIGHT,
                          5, laneColor(noteLane));
    display.drawRoundRect(laneX + 8, noteTop, LANE_WIDTH - 16, NOTE_HEIGHT,
                          5, TFT_WHITE);
  }

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
    uint8_t melodyIndex = 0;
    bool noteActive = false;
    uint8_t noteLane = 0;
    float noteY = 0.0f;
    uint32_t nextNoteAt = millis() + FIRST_NOTE_DELAY_MS;
    uint32_t lastFrame = millis();
    uint32_t button4HeldSince = 0;
    bool gameOver = false;

    while (!gameOver) {
      const uint32_t now = millis();
      if (now - lastFrame < FRAME_MS) {
        delay(2);
        continue;
      }
      const uint32_t elapsedMs = now - lastFrame;
      lastFrame = now;

      const Button button = readButton();
      if (button == Button::Button4) {
        if (button4HeldSince == 0) {
          button4HeldSince = now;
        } else if (now - button4HeldSince >= BUTTON4_BACK_HOLD_MS) {
          audioEnd();
          waitForButtonRelease();
          return;
        }
      } else {
        button4HeldSince = 0;
      }

      if (!noteActive && now >= nextNoteAt) {
        noteActive = true;
        noteY = static_cast<float>(LANE_TOP + NOTE_HEIGHT / 2);
        noteLane = MARY_MELODY[melodyIndex].lane;
      }
      if (noteActive) {
        noteY += NOTE_SPEED * elapsedMs;
      }

      const int8_t pressedLane = buttonLane(button);
      if (noteActive && pressedLane >= 0 &&
          pressedLane == static_cast<int8_t>(noteLane)) {
        const float distance =
            noteY > HIT_LINE_Y ? noteY - HIT_LINE_Y : HIT_LINE_Y - noteY;
        if (distance <= HIT_WINDOW) {
          ++combo;
          misses = 0;
          score += static_cast<uint16_t>(100 + combo * 10);
          noteActive = false;
          nextNoteAt = now + NOTE_INTERVAL_MS;
          playMelodyNote(MARY_MELODY[melodyIndex].frequency);
          melodyIndex = (melodyIndex + 1) % MARY_MELODY_LENGTH;
        }
      }

      if (noteActive && noteY > HIT_LINE_Y + HIT_WINDOW) {
        noteActive = false;
        combo = 0;
        ++misses;
        nextNoteAt = now + NOTE_INTERVAL_MS;
        playMissSound();
        melodyIndex = (melodyIndex + 1) % MARY_MELODY_LENGTH;
        if (misses > MAX_CONSECUTIVE_MISSES) {
          gameOver = true;
        }
      }

      if (useSprite) {
        drawFrame(gameFrame, noteY, noteActive, noteLane, score, combo,
                  misses);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, noteY, noteActive, noteLane, score, combo, misses);
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

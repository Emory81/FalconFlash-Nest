#include "flappy_bird.h"

#include <Arduino.h>

#include <cstdio>

#include "audio_helper.h"
#include "button_input.h"

namespace {
constexpr int16_t GROUND_HEIGHT = 18;
constexpr int16_t PIPE_WIDTH = 28;
constexpr int16_t GAP_HEIGHT = 76;
constexpr int16_t BIRD_X = 58;
constexpr float GRAVITY = 0.24f;
constexpr float FLAP_VELOCITY = -4.8f;
constexpr float PIPE_SPEED = 2.6f;
constexpr int16_t BACK_BUTTON_X = 8;
constexpr int16_t BACK_BUTTON_Y = 208;
constexpr int16_t BACK_BUTTON_W = 66;
constexpr int16_t BACK_BUTTON_H = 25;

void playFlapSound() {
  audioBegin(false);
  audioPlayTone(880, 18);
  audioEnd();
}

void playScoreSound() {
  audioBegin(false);
  audioPlayTone(1175, 35);
  delay(8);
  audioPlayTone(1568, 45);
  audioEnd();
}

void playGameOverSound() {
  audioBegin(false);
  audioPlayTone(440, 100);
  delay(20);
  audioPlayTone(330, 140);
  audioEnd();
}

void drawBackButton(TFT_eSPI& display) {
  display.fillRoundRect(BACK_BUTTON_X, BACK_BUTTON_Y, BACK_BUTTON_W,
                        BACK_BUTTON_H, 6, TFT_BLACK);
  display.drawRoundRect(BACK_BUTTON_X, BACK_BUTTON_Y, BACK_BUTTON_W,
                        BACK_BUTTON_H, 6, TFT_WHITE);
  display.setTextFont(1);
  display.setTextSize(1);
  display.setTextColor(TFT_WHITE, TFT_BLACK);
  display.setTextDatum(MC_DATUM);
  display.drawString("BTN4 Back", BACK_BUTTON_X + BACK_BUTTON_W / 2,
                     BACK_BUTTON_Y + BACK_BUTTON_H / 2);
}

void drawFrame(TFT_eSPI& display, float birdY, int16_t pipeX, int16_t gapTop,
               uint16_t score) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();
  const int16_t gapBottom = gapTop + GAP_HEIGHT;

  display.fillRect(0, 0, screenW, screenH, TFT_SKYBLUE);
  display.fillRect(0, screenH - GROUND_HEIGHT, screenW, GROUND_HEIGHT,
                   TFT_DARKGREEN);
  display.fillRect(pipeX, 0, PIPE_WIDTH, gapTop, TFT_GREEN);
  display.fillRect(pipeX, gapBottom, PIPE_WIDTH,
                   screenH - GROUND_HEIGHT - gapBottom, TFT_GREEN);
  display.drawRect(pipeX, 0, PIPE_WIDTH, gapTop, TFT_DARKGREEN);
  display.drawRect(pipeX, gapBottom, PIPE_WIDTH,
                   screenH - GROUND_HEIGHT - gapBottom, TFT_DARKGREEN);

  display.fillCircle(BIRD_X, static_cast<int16_t>(birdY), 10, TFT_YELLOW);
  display.drawCircle(BIRD_X, static_cast<int16_t>(birdY), 10, TFT_BLACK);
  display.fillCircle(BIRD_X + 4, static_cast<int16_t>(birdY) - 3, 2,
                     TFT_BLACK);

  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_SKYBLUE);
  display.setTextDatum(TL_DATUM);
  char scoreText[16];
  snprintf(scoreText, sizeof(scoreText), "Score: %u", score);
  display.drawString(scoreText, 8, 6);
  drawBackButton(display);
}

void drawGameOver(TFT_eSPI& display) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();
  display.fillRect(0, 0, screenW, screenH, TFT_SKYBLUE);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, TFT_SKYBLUE);
  display.setTextDatum(MC_DATUM);
  display.drawString("Game Over", screenW / 2, screenH / 2 - 18);
  display.setTextFont(1);
  display.setTextSize(1);
  display.drawString("BTN3 to play again", screenW / 2, screenH / 2 + 22);
  drawBackButton(display);
}
}  // namespace

void runFlappyBird(TFT_eSPI& display) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();

  waitForButtonRelease();

  TFT_eSprite gameFrame(&display);
  gameFrame.setColorDepth(8);
  gameFrame.createSprite(screenW, screenH);
  const bool useSprite = gameFrame.created();

  while (true) {
    float birdY = screenH / 2.0f;
    float birdVelocity = 0.0f;
    float pipeX = screenW + 20.0f;
    int16_t gapTop = random(35, screenH - GROUND_HEIGHT - GAP_HEIGHT - 20);
    uint16_t score = 0;
    bool pipeScored = false;
    uint32_t lastFrame = millis();

    while (true) {
      const uint32_t now = millis();
      if (now - lastFrame < 30) {
        delay(2);
        continue;
      }
      lastFrame = now;

      const Button button = readButtonEvent();
      if (button == Button::Button4) {
        waitForButtonRelease();
        return;
      }
      if (button == Button::Button1) {
        birdVelocity = FLAP_VELOCITY;
        playFlapSound();
      }

      birdVelocity += GRAVITY;
      birdY += birdVelocity;
      pipeX -= PIPE_SPEED;

      if (!pipeScored && pipeX + PIPE_WIDTH < BIRD_X) {
        ++score;
        pipeScored = true;
        playScoreSound();
      }
      if (pipeX + PIPE_WIDTH < 0) {
        pipeX = screenW + 20.0f;
        gapTop = random(35, screenH - GROUND_HEIGHT - GAP_HEIGHT - 20);
        pipeScored = false;
      }

      const int16_t gapBottom = gapTop + GAP_HEIGHT;
      const bool outsideScreen =
          birdY - 10 < 0 || birdY + 10 >= screenH - GROUND_HEIGHT;
      const bool inPipeX =
          BIRD_X + 10 > pipeX && BIRD_X - 10 < pipeX + PIPE_WIDTH;
      const bool hitPipe =
          inPipeX && (birdY - 10 < gapTop || birdY + 10 > gapBottom);
      if (outsideScreen || hitPipe) {
        break;
      }

      if (useSprite) {
        drawFrame(gameFrame, birdY, static_cast<int16_t>(pipeX), gapTop,
                  score);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, birdY, static_cast<int16_t>(pipeX), gapTop, score);
      }
    }

    if (useSprite) {
      drawGameOver(gameFrame);
      gameFrame.pushSprite(0, 0);
    } else {
      drawGameOver(display);
    }
    playGameOverSound();

    while (true) {
      const Button button = readButtonEvent();
      if (button == Button::Button4) {
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

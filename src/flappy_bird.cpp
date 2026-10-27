#include "flappy_bird.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>

#include "audio_helper.h"
#include "button_input.h"
#include "flappy_sprites.h"

namespace {
constexpr int16_t GROUND_HEIGHT = 18;
constexpr int16_t PIPE_WIDTH = 28;
constexpr int16_t GAP_HEIGHT = 76;
constexpr int16_t BIRD_X = 58;
// Hit detection uses only the body: a box centered on the bird's position,
// independent of the sprite size, so wings can stick out without colliding.
// Tune these to match the body in the art; set SHOW_HITBOX to see the box.
constexpr int16_t BIRD_BODY_HALF_W = 6;
constexpr int16_t BIRD_BODY_HALF_H = 6;
constexpr int16_t BIRD_BODY_OFFSET_X = 0;
constexpr int16_t BIRD_BODY_OFFSET_Y = 0;
constexpr bool SHOW_HITBOX = false;
// Milliseconds each wing frame is shown; lower is a faster flap.
// Nose-up/down tilt is limited to this angle (degrees).
constexpr float MAX_TILT_DEGREES = 35.0f;
// Fraction of the true travel angle to show (1.0 = exact).
constexpr float TILT_SCALE = 0.6f;
constexpr uint32_t FLAP_FRAME_MS = 110;
constexpr float GRAVITY = 0.24f;
constexpr float FLAP_VELOCITY = -4.8f;
constexpr float PIPE_SPEED = 2.6f;
constexpr int16_t BACK_BUTTON_X = 8;
constexpr int16_t BACK_BUTTON_Y = 208;
constexpr int16_t BACK_BUTTON_W = 66;
constexpr int16_t BACK_BUTTON_H = 25;

void playFlapSound() {
  audioPlayTone(880, 18);
}

void playScoreSound() {
  audioPlayTone(1175, 35);
  delay(8);
  audioPlayTone(1568, 45);
}

void playGameOverSound() {
  audioPlayTone(440, 100);
  delay(20);
  audioPlayTone(330, 140);
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

// One flap per button press: open, transition, closed, then back to the
// default wings-back pose.
const uint16_t* birdFrame(uint32_t flapStartedAt) {
  const uint16_t* const flap[] = {birdFlapOpen, birdFlapTransition,
                                  birdFlapClosed};
  const uint32_t step = (millis() - flapStartedAt) / FLAP_FRAME_MS;
  return step < 3 ? flap[step] : birdWingsBack;
}

// TFT_eSprite has no transparent pushImage(), so skip see-through pixels by
// hand. drawPixel() is virtual, so this works on the sprite and the screen.
// The art faces right; tilt is clockwise on screen (nose down when positive),
// centered on (centerX, centerY), sampled nearest-neighbor.
void drawSpriteRotated(TFT_eSPI& display, int16_t centerX, int16_t centerY,
                       int16_t w, int16_t h, const uint16_t* pixels,
                       float tiltRadians) {
  const float cosT = cosf(tiltRadians);
  const float sinT = sinf(tiltRadians);
  const float srcCenterX = (w - 1) / 2.0f;
  const float srcCenterY = (h - 1) / 2.0f;
  const int16_t radius = static_cast<int16_t>(sqrtf(w * w + h * h) / 2.0f) + 1;

  for (int16_t dy = -radius; dy <= radius; ++dy) {
    for (int16_t dx = -radius; dx <= radius; ++dx) {
      const int16_t sx = lroundf(srcCenterX + dx * cosT + dy * sinT);
      const int16_t sy = lroundf(srcCenterY - dx * sinT + dy * cosT);
      if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
        continue;
      }
      const uint16_t color = pixels[sy * w + sx];
      if (color != SPRITE_TRANSPARENT) {
        display.drawPixel(centerX + dx, centerY + dy, color);
      }
    }
  }
}

void drawFrame(TFT_eSPI& display, float birdY, float birdVelocity,
               int16_t pipeX, int16_t gapTop, uint16_t score,
               uint32_t flapStartedAt) {
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

  // Direction of travel: pipes approach at PIPE_SPEED while the bird moves
  // vertically at birdVelocity.
  float tilt = atan2f(birdVelocity, PIPE_SPEED) * TILT_SCALE;
  const float maxTilt = MAX_TILT_DEGREES * static_cast<float>(M_PI) / 180.0f;
  tilt = tilt > maxTilt ? maxTilt : (tilt < -maxTilt ? -maxTilt : tilt);
  drawSpriteRotated(display, BIRD_X, static_cast<int16_t>(birdY),
                    BIRD_FLAP_OPEN_WIDTH, BIRD_FLAP_OPEN_HEIGHT,
                    birdFrame(flapStartedAt), tilt);
  if (SHOW_HITBOX) {
    display.drawRect(
        BIRD_X + BIRD_BODY_OFFSET_X - BIRD_BODY_HALF_W,
        static_cast<int16_t>(birdY) + BIRD_BODY_OFFSET_Y - BIRD_BODY_HALF_H,
        BIRD_BODY_HALF_W * 2, BIRD_BODY_HALF_H * 2, TFT_RED);
  }

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
  // Keep the speaker enabled while the game runs so short effects are heard.
  audioBegin();

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
    uint32_t flapStartedAt = lastFrame - 10 * FLAP_FRAME_MS;  // start at rest

    while (true) {
      const uint32_t now = millis();
      if (now - lastFrame < 30) {
        delay(2);
        continue;
      }
      lastFrame = now;

      const Button button = readButtonEvent();
      if (button == Button::Button4) {
        audioEnd();
        gameFrame.deleteSprite();
        waitForButtonRelease();
        return;
      }
      if (button == Button::Button1) {
        birdVelocity = FLAP_VELOCITY;
        flapStartedAt = now;
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
      const float bodyLeft = BIRD_X + BIRD_BODY_OFFSET_X - BIRD_BODY_HALF_W;
      const float bodyRight = BIRD_X + BIRD_BODY_OFFSET_X + BIRD_BODY_HALF_W;
      const float bodyTop = birdY + BIRD_BODY_OFFSET_Y - BIRD_BODY_HALF_H;
      const float bodyBottom = birdY + BIRD_BODY_OFFSET_Y + BIRD_BODY_HALF_H;
      const bool outsideScreen =
          bodyTop < 0 || bodyBottom >= screenH - GROUND_HEIGHT;
      const bool inPipeX = bodyRight > pipeX && bodyLeft < pipeX + PIPE_WIDTH;
      const bool hitPipe = inPipeX && (bodyTop < gapTop || bodyBottom > gapBottom);
      if (outsideScreen || hitPipe) {
        break;
      }

      if (useSprite) {
        drawFrame(gameFrame, birdY, birdVelocity, static_cast<int16_t>(pipeX),
                  gapTop, score, flapStartedAt);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, birdY, birdVelocity, static_cast<int16_t>(pipeX),
                  gapTop, score, flapStartedAt);
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
        audioEnd();
        gameFrame.deleteSprite();
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

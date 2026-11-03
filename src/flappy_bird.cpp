#include "flappy_bird.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio_helper.h"
#include "button_input.h"
#include "flappy_sprites.h"

namespace {
// The background image fills the whole screen, so there is no ground strip.
constexpr int16_t GROUND_HEIGHT = 0;
// Pipe = stem (PipeBody art, repeated) + a slightly wider cap (PipeCap art)
// on the open end, so the stem width and cap overhang come from the art.
constexpr int16_t PIPE_WIDTH = PIPE_BODY_WIDTH;
constexpr int16_t PIPE_CAP_OVERHANG = (PIPE_CAP_WIDTH - PIPE_BODY_WIDTH) / 2;
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
// Nose-up/down tilt is limited to this angle (degrees).
constexpr float MAX_TILT_DEGREES = 35.0f;
// Fraction of the true travel angle to show (1.0 = exact).
constexpr float TILT_SCALE = 0.6f;
// Milliseconds each wing frame is shown; lower is a faster flap.
constexpr uint32_t FLAP_FRAME_MS = 110;
constexpr float GRAVITY = 0.24f;
constexpr float FLAP_VELOCITY = -4.8f;
constexpr float PIPE_SPEED = 2.6f;
constexpr uint8_t PIPE_COUNT = 2;
constexpr float PIPE_SPAWN_OFFSET = 20.0f;
constexpr float PIPE_SPACING_SCALE = 0.5f;
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

// Full-screen background. backgroundImage is RGB332, the same layout as the
// 8-bit sprite's buffer, so the sprite path is a single memcpy. Without a
// sprite it goes straight to the screen.
void drawBackground(TFT_eSPI& display, TFT_eSprite* sprite) {
  // The image must match the screen exactly (re-run tools/convert_sprites.py
  // if the display size changes); otherwise rows would be misaligned.
  if (display.width() != BACKGROUND_WIDTH ||
      display.height() != BACKGROUND_HEIGHT) {
    TFT_eSPI& canvas = sprite != nullptr ? *sprite : display;
    canvas.fillRect(0, 0, display.width(), display.height(), TFT_BLACK);
    return;
  }
  if (sprite != nullptr) {
    memcpy(sprite->getPointer(), backgroundImage, sizeof(backgroundImage));
  } else {
    display.pushImage(0, 0, BACKGROUND_WIDTH, BACKGROUND_HEIGHT,
                      const_cast<uint8_t*>(backgroundImage), true);
  }
}

// Draws an opaque/transparent image with clipping, optionally flipped
// vertically (for the cap of the pipe hanging from the top).
void drawImage(TFT_eSPI& display, int16_t x, int16_t y, int16_t w, int16_t h,
               const uint16_t* pixels, bool flipVertical) {
  for (int16_t row = 0; row < h; ++row) {
    const int16_t dy = y + row;
    if (dy < 0 || dy >= display.height()) {
      continue;
    }
    const int16_t srcRow = flipVertical ? h - 1 - row : row;
    for (int16_t col = 0; col < w; ++col) {
      const int16_t dx = x + col;
      if (dx < 0 || dx >= display.width()) {
        continue;
      }
      const uint16_t color = pixels[srcRow * w + col];
      if (color != SPRITE_TRANSPARENT) {
        display.drawPixel(dx, dy, color);
      }
    }
  }
}

// Stem rows [fromY, toY) from the repeating body art, then the cap.
void drawPipe(TFT_eSPI& display, int16_t pipeX, int16_t gapTop,
              int16_t gapBottom) {
  const int16_t screenH = display.height();
  const int16_t capX = pipeX - PIPE_CAP_OVERHANG;

  // Top pipe: stem from the ceiling down to its cap.
  for (int16_t y = 0; y < gapTop - PIPE_CAP_HEIGHT; y += PIPE_BODY_HEIGHT) {
    const int16_t h = min<int16_t>(PIPE_BODY_HEIGHT, gapTop - PIPE_CAP_HEIGHT - y);
    drawImage(display, pipeX, y, PIPE_BODY_WIDTH, h, pipeBody, false);
  }
  drawImage(display, capX, gapTop - PIPE_CAP_HEIGHT, PIPE_CAP_WIDTH,
            PIPE_CAP_HEIGHT, pipeCap, true);

  // Bottom pipe: cap at the gap edge, stem below it to the bottom of the screen.
  drawImage(display, capX, gapBottom, PIPE_CAP_WIDTH, PIPE_CAP_HEIGHT, pipeCap,
            false);
  for (int16_t y = gapBottom + PIPE_CAP_HEIGHT; y < screenH;
       y += PIPE_BODY_HEIGHT) {
    drawImage(display, pipeX, y, PIPE_BODY_WIDTH, PIPE_BODY_HEIGHT, pipeBody,
              false);
  }
}

void drawFrame(TFT_eSPI& display, float birdY, float birdVelocity,
               const float* pipeX, const int16_t* gapTop, uint16_t score,
               uint32_t flapStartedAt) {
  const int16_t screenH = display.height();
  for (uint8_t index = 0; index < PIPE_COUNT; ++index) {
    const int16_t pipeGapBottom = gapTop[index] + GAP_HEIGHT;
    drawPipe(display, static_cast<int16_t>(pipeX[index]), gapTop[index],
             pipeGapBottom);
  }

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
  display.setTextColor(TFT_WHITE);
  display.setTextDatum(TL_DATUM);
  char scoreText[16];
  snprintf(scoreText, sizeof(scoreText), "Score: %u", score);
  display.drawString(scoreText, 8, 6);
  drawBackButton(display);
}

void drawGameOver(TFT_eSPI& display) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextColor(TFT_WHITE);
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
  TFT_eSprite* const sprite = useSprite ? &gameFrame : nullptr;

  while (true) {
    float birdY = screenH / 2.0f;
    float birdVelocity = 0.0f;
    const float pipeSpacing =
        (screenW + PIPE_SPAWN_OFFSET) * PIPE_SPACING_SCALE;
    float pipeX[PIPE_COUNT];
    int16_t gapTop[PIPE_COUNT];
    bool pipeScored[PIPE_COUNT] = {};
    for (uint8_t index = 0; index < PIPE_COUNT; ++index) {
      pipeX[index] =
          screenW + PIPE_SPAWN_OFFSET + index * pipeSpacing;
      gapTop[index] =
          random(35, screenH - GROUND_HEIGHT - GAP_HEIGHT - 20);
    }
    uint16_t score = 0;
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
      for (uint8_t index = 0; index < PIPE_COUNT; ++index) {
        pipeX[index] -= PIPE_SPEED;

        if (!pipeScored[index] && pipeX[index] + PIPE_WIDTH < BIRD_X) {
          ++score;
          pipeScored[index] = true;
          playScoreSound();
        }
        if (pipeX[index] + PIPE_WIDTH < 0) {
          float furthestPipeX = pipeX[index];
          for (uint8_t other = 0; other < PIPE_COUNT; ++other) {
            if (other != index && pipeX[other] > furthestPipeX) {
              furthestPipeX = pipeX[other];
            }
          }
          pipeX[index] = furthestPipeX + pipeSpacing;
          gapTop[index] =
              random(35, screenH - GROUND_HEIGHT - GAP_HEIGHT - 20);
          pipeScored[index] = false;
        }
      }

      const float bodyLeft = BIRD_X + BIRD_BODY_OFFSET_X - BIRD_BODY_HALF_W;
      const float bodyRight = BIRD_X + BIRD_BODY_OFFSET_X + BIRD_BODY_HALF_W;
      const float bodyTop = birdY + BIRD_BODY_OFFSET_Y - BIRD_BODY_HALF_H;
      const float bodyBottom = birdY + BIRD_BODY_OFFSET_Y + BIRD_BODY_HALF_H;
      const bool outsideScreen =
          bodyTop < 0 || bodyBottom >= screenH - GROUND_HEIGHT;
      bool hitPipe = false;
      for (uint8_t index = 0; index < PIPE_COUNT; ++index) {
        const int16_t pipeGapBottom = gapTop[index] + GAP_HEIGHT;
        const bool inPipeX =
            bodyRight > pipeX[index] &&
            bodyLeft < pipeX[index] + PIPE_WIDTH;
        // The cap sticks out past the stem, so it is checked over its own rows.
        const bool inCapX =
            bodyRight > pipeX[index] - PIPE_CAP_OVERHANG &&
            bodyLeft < pipeX[index] + PIPE_WIDTH + PIPE_CAP_OVERHANG;
        const bool hitCap =
            inCapX &&
            ((bodyTop < gapTop[index] &&
              bodyBottom > gapTop[index] - PIPE_CAP_HEIGHT) ||
             (bodyBottom > pipeGapBottom &&
              bodyTop < pipeGapBottom + PIPE_CAP_HEIGHT));
        if (hitCap ||
            (inPipeX &&
             (bodyTop < gapTop[index] || bodyBottom > pipeGapBottom))) {
          hitPipe = true;
          break;
        }
      }
      if (outsideScreen || hitPipe) {
        break;
      }

      drawBackground(display, sprite);
      if (useSprite) {
        drawFrame(gameFrame, birdY, birdVelocity, pipeX, gapTop, score,
                  flapStartedAt);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, birdY, birdVelocity, pipeX, gapTop, score,
                  flapStartedAt);
      }
    }

    drawBackground(display, sprite);
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

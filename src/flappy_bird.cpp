#include "flappy_bird.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "audio_helper.h"
#include "button_input.h"
#include "flappy_sprites.h"
#include "leaderboard.h"

namespace {
// The bird art and the pipes are enlarged by these factors; the gap, speeds
// and gravity stay in screen pixels. The big bird art is already drawn large
// (48x48), so it is not scaled further.
constexpr float ZOOM = 1.0f;
constexpr float PIPE_ZOOM = 1.25f;
constexpr int16_t zoomed(int16_t value) {
  return static_cast<int16_t>(value * ZOOM + 0.5f);
}
constexpr int16_t pipeZoomed(int16_t value) {
  return static_cast<int16_t>(value * PIPE_ZOOM + 0.5f);
}
// The background image fills the whole screen, so there is no ground strip.
constexpr int16_t GROUND_HEIGHT = 0;
// Pipe = stem (PipeBody art, repeated) + a slightly wider cap (PipeCap art)
// on the open end, so the stem width and cap overhang come from the art.
constexpr int16_t PIPE_WIDTH = pipeZoomed(PIPE_BODY_WIDTH);
constexpr int16_t PIPE_TILE_HEIGHT = pipeZoomed(PIPE_BODY_HEIGHT);
constexpr int16_t PIPE_CAP_W = pipeZoomed(PIPE_CAP_WIDTH);
constexpr int16_t PIPE_CAP_H = pipeZoomed(PIPE_CAP_HEIGHT);
constexpr int16_t PIPE_CAP_OVERHANG = (PIPE_CAP_W - PIPE_WIDTH) / 2;
constexpr int16_t GAP_HEIGHT = 76;
constexpr int16_t BIRD_X = 58;
// Hit detection uses only the body: a box centered on the bird's position,
// independent of the sprite size, so wings can stick out without colliding.
// Tune these to match the body in the art; set SHOW_HITBOX to see the box.
constexpr int16_t BIRD_BODY_HALF_W = 9;
constexpr int16_t BIRD_BODY_HALF_H = 9;
constexpr int16_t BIRD_BODY_OFFSET_X = 0;
constexpr int16_t BIRD_BODY_OFFSET_Y = 0;
constexpr bool SHOW_HITBOX = false;
// Nose-up/down tilt is limited to this angle (degrees).
constexpr float MAX_TILT_DEGREES = 35.0f;
// Fraction of the true travel angle to show (1.0 = exact).
constexpr float TILT_SCALE = 0.6f;
// Milliseconds each wing frame is shown; lower is a faster flap.
constexpr uint32_t FLAP_FRAME_MS = 110;
// One constant gravity for the whole arc, so the rise and fall are symmetric.
constexpr float GRAVITY = 0.96f;
// Extra downward acceleration per frame while button 2 is held, and the cap.
constexpr float DIVE_ACCEL = 2.0f;
constexpr float DIVE_MAX_VELOCITY = 16.0f;
// One game in this many is played at night: night background and pipes
// NIGHT_PIPE_FREQUENCY_MULTIPLIER times more common.
constexpr long NIGHT_ONE_IN = 5;
constexpr float NIGHT_PIPE_FREQUENCY_MULTIPLIER = 1.5f;
constexpr float FLAP_VELOCITY = -9.8f;
// Forward speed (pixels per frame). Starts 1.5x the original 2.6 and speeds up
// after every pipe passed, up to a cap.
constexpr float PIPE_SPEED_START = 6.2f;
constexpr float PIPE_SPEED_STEP = 0.2f;
constexpr float PIPE_SPEED_MAX = 9.2f;
// How many times more often pipes appear compared to the original layout of
// one pipe at a time (spacing was screen width + margin + pipe width).
constexpr float PIPE_FREQUENCY = 1.5f;
constexpr int16_t PIPE_SPAWN_MARGIN = 20;
constexpr uint8_t MAX_PIPES = 4;

struct Pipe {
  float x;
  int16_t gapTop;
  bool scored;
};
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
  const int16_t radius =
      static_cast<int16_t>(sqrtf(w * w + h * h) * ZOOM / 2.0f) + 1;

  for (int16_t dy = -radius; dy <= radius; ++dy) {
    for (int16_t dx = -radius; dx <= radius; ++dx) {
      const int16_t sx = lroundf(srcCenterX + (dx * cosT + dy * sinT) / ZOOM);
      const int16_t sy = lroundf(srcCenterY + (dy * cosT - dx * sinT) / ZOOM);
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

// Full-screen background, day or night. The images are RGB332, the same
// layout as the 8-bit sprite's buffer, so the sprite path is a single memcpy.
// Without a sprite the day image goes straight to the screen.
void drawBackground(TFT_eSPI& display, TFT_eSprite* sprite, bool night) {
  // The image must match the screen exactly (re-run tools/convert_sprites.py
  // if the display size changes); otherwise rows would be misaligned.
  if (display.width() != BACKGROUND_WIDTH ||
      display.height() != BACKGROUND_HEIGHT) {
    TFT_eSPI& canvas = sprite != nullptr ? *sprite : display;
    canvas.fillRect(0, 0, display.width(), display.height(), TFT_BLACK);
    return;
  }
  const uint8_t* image = night ? backgroundNightImage : backgroundImage;
  if (sprite != nullptr) {
    memcpy(sprite->getPointer(), image, sizeof(backgroundImage));
  } else {
    display.pushImage(0, 0, BACKGROUND_WIDTH, BACKGROUND_HEIGHT,
                      const_cast<uint8_t*>(image), true);
  }
}

// Draws an image scaled to destW x destH (nearest neighbor) with clipping,
// optionally flipped vertically (for the cap of the pipe hanging from the
// top). Transparent pixels are skipped.
void drawImage(TFT_eSPI& display, int16_t x, int16_t y, int16_t srcW,
               int16_t srcH, const uint16_t* pixels, bool flipVertical,
               int16_t destW, int16_t destH) {
  for (int16_t row = 0; row < destH; ++row) {
    const int16_t dy = y + row;
    if (dy < 0 || dy >= display.height()) {
      continue;
    }
    int16_t srcRow = static_cast<int16_t>(row / PIPE_ZOOM);
    srcRow = srcRow >= srcH ? srcH - 1 : srcRow;
    if (flipVertical) {
      srcRow = srcH - 1 - srcRow;
    }
    for (int16_t col = 0; col < destW; ++col) {
      const int16_t dx = x + col;
      if (dx < 0 || dx >= display.width()) {
        continue;
      }
      int16_t srcCol = static_cast<int16_t>(col / PIPE_ZOOM);
      srcCol = srcCol >= srcW ? srcW - 1 : srcCol;
      const uint16_t color = pixels[srcRow * srcW + srcCol];
      if (color != SPRITE_TRANSPARENT) {
        display.drawPixel(dx, dy, color);
      }
    }
  }
}

// Stem tiles from the repeating body art, then the cap on the open end.
void drawPipe(TFT_eSPI& display, int16_t pipeX, int16_t gapTop,
              int16_t gapBottom) {
  const int16_t screenH = display.height();
  const int16_t capX = pipeX - PIPE_CAP_OVERHANG;

  // Top pipe: stem from the ceiling down to its cap.
  for (int16_t y = 0; y < gapTop - PIPE_CAP_H; y += PIPE_TILE_HEIGHT) {
    const int16_t h = min<int16_t>(PIPE_TILE_HEIGHT, gapTop - PIPE_CAP_H - y);
    drawImage(display, pipeX, y, PIPE_BODY_WIDTH, PIPE_BODY_HEIGHT, pipeBody,
              false, PIPE_WIDTH, h);
  }
  drawImage(display, capX, gapTop - PIPE_CAP_H, PIPE_CAP_WIDTH,
            PIPE_CAP_HEIGHT, pipeCap, true, PIPE_CAP_W, PIPE_CAP_H);

  // Bottom pipe: cap at the gap edge, stem below it to the bottom of the screen.
  drawImage(display, capX, gapBottom, PIPE_CAP_WIDTH, PIPE_CAP_HEIGHT, pipeCap,
            false, PIPE_CAP_W, PIPE_CAP_H);
  for (int16_t y = gapBottom + PIPE_CAP_H; y < screenH; y += PIPE_TILE_HEIGHT) {
    drawImage(display, pipeX, y, PIPE_BODY_WIDTH, PIPE_BODY_HEIGHT, pipeBody,
              false, PIPE_WIDTH, PIPE_TILE_HEIGHT);
  }
}

void drawFrame(TFT_eSPI& display, float birdY, float birdVelocity,
               float pipeSpeed, const Pipe* pipes, uint8_t pipeCount,
               uint16_t score, uint32_t best, uint32_t flapStartedAt) {
  for (uint8_t i = 0; i < pipeCount; ++i) {
    drawPipe(display, static_cast<int16_t>(pipes[i].x), pipes[i].gapTop,
             pipes[i].gapTop + GAP_HEIGHT);
  }

  // Direction of travel: pipes approach at pipeSpeed while the bird moves
  // vertically at birdVelocity.
  float tilt = atan2f(birdVelocity, pipeSpeed) * TILT_SCALE;
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
  display.setTextDatum(TL_DATUM);
  char scoreText[16];
  snprintf(scoreText, sizeof(scoreText), "Score: %u", score);
  const int16_t boxW = display.textWidth(scoreText) + 12;
  const int16_t boxH = display.fontHeight() + 8;
  display.fillRoundRect(4, 2, boxW, boxH, 6, TFT_BLACK);
  display.drawRoundRect(4, 2, boxW, boxH, 6, TFT_WHITE);
  display.setTextColor(score > best ? TFT_YELLOW : TFT_WHITE);
  display.drawString(scoreText, 10, 6);
  drawBackButton(display);
}

// Game-over popup over the background: this game's score, the device best and
// the leaderboard, with the score just earned highlighted in the list.
void drawGameOver(TFT_eSPI& display, uint32_t score, uint32_t best,
                  bool newBest, const uint32_t* top, uint8_t topCount,
                  uint32_t totalFlaps) {
  constexpr uint16_t PANEL_BG = 0x18C7;
  constexpr int16_t PANEL_W = 210;
  constexpr int16_t PANEL_H = 224;
  const int16_t centerX = display.width() / 2;
  const int16_t panelX = centerX - PANEL_W / 2;
  const int16_t panelY = (display.height() - PANEL_H) / 2;

  display.fillRoundRect(panelX, panelY, PANEL_W, PANEL_H, 10, PANEL_BG);
  display.drawRoundRect(panelX, panelY, PANEL_W, PANEL_H, 10, TFT_WHITE);
  display.setTextFont(1);
  display.setTextDatum(TC_DATUM);

  display.setTextSize(2);
  display.setTextColor(TFT_WHITE, PANEL_BG);
  display.drawString("Game Over", centerX, panelY + 10);

  char line[32];
  snprintf(line, sizeof(line), "Score: %lu", static_cast<unsigned long>(score));
  display.drawString(line, centerX, panelY + 38);
  snprintf(line, sizeof(line), "Best: %lu", static_cast<unsigned long>(best));
  display.setTextColor(TFT_YELLOW, PANEL_BG);
  display.drawString(line, centerX, panelY + 60);
  if (newBest) {
    display.setTextSize(1);
    display.drawString("NEW BEST!", centerX, panelY + 80);
  }

  display.setTextSize(1);
  display.setTextColor(TFT_CYAN, PANEL_BG);
  display.drawString("Leaderboard", centerX, panelY + 98);
  bool highlighted = false;
  for (uint8_t i = 0; i < LEADERBOARD_SIZE; ++i) {
    const int16_t rowY = panelY + 114 + i * 14;
    if (i < topCount) {
      // Highlight only the first row matching this game's score.
      const bool isThisGame = !highlighted && top[i] == score;
      highlighted = highlighted || isThisGame;
      display.setTextColor(isThisGame ? TFT_YELLOW : TFT_WHITE, PANEL_BG);
      snprintf(line, sizeof(line), "%u.  %lu", i + 1,
               static_cast<unsigned long>(top[i]));
    } else {
      display.setTextColor(TFT_DARKGREY, PANEL_BG);
      snprintf(line, sizeof(line), "%u.  ---", i + 1);
    }
    display.drawString(line, centerX, rowY);
  }

  // Lifetime flaps, shown under the leaderboard.
  display.setTextColor(TFT_CYAN, PANEL_BG);
  snprintf(line, sizeof(line), "Total flaps: %lu",
           static_cast<unsigned long>(totalFlaps));
  display.drawString(line, centerX, panelY + 186);

  display.setTextColor(TFT_WHITE, PANEL_BG);
  display.drawString("BTN3 Play again   BTN4 Back", centerX,
                     panelY + PANEL_H - 18);
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
    Pipe pipes[MAX_PIPES];
    uint8_t pipeCount = 0;
    const bool night = random(NIGHT_ONE_IN) == 0;
    const float pipeFrequency =
        night ? PIPE_FREQUENCY * NIGHT_PIPE_FREQUENCY_MULTIPLIER
              : PIPE_FREQUENCY;
    const float pipeSpacing =
        (screenW + PIPE_SPAWN_MARGIN + PIPE_WIDTH) / pipeFrequency;
    uint16_t score = 0;
    const uint32_t bestAtStart = leaderboardBest("flappy");
    float pipeSpeed = PIPE_SPEED_START;
    uint32_t lastFrame = millis();
    uint32_t flapStartedAt = lastFrame - 10 * FLAP_FRAME_MS;  // start at rest
    uint32_t roundFlaps = 0;  // added to the lifetime counter when the round ends

    while (true) {
      const uint32_t now = millis();
      if (now - lastFrame < 30) {
        delay(2);
        continue;
      }
      lastFrame = now;

      const Button button = readButtonEvent();
      if (button == Button::Button4) {
        leaderboardAddFlaps("flappy", roundFlaps);  // quitting still counts
        audioEnd();
        gameFrame.deleteSprite();
        waitForButtonRelease();
        return;
      }
      const Button held = readButton();
      // A press flaps at once; holding it flaps again each time the wing
      // animation finishes.
      if (button == Button::Button1 ||
          (held == Button::Button1 && now - flapStartedAt >= 3 * FLAP_FRAME_MS)) {
        birdVelocity = FLAP_VELOCITY;
        flapStartedAt = now;
        ++roundFlaps;
        playFlapSound();
      }

      birdVelocity += GRAVITY;
      if (held == Button::Button2) {
        birdVelocity += DIVE_ACCEL;
        if (birdVelocity > DIVE_MAX_VELOCITY) birdVelocity = DIVE_MAX_VELOCITY;
      }
      birdY += birdVelocity;

      for (uint8_t i = 0; i < pipeCount; ++i) {
        pipes[i].x -= pipeSpeed;
      }

      // Drop pipes that left the screen, then add one when there is room.
      while (pipeCount > 0 && pipes[0].x + PIPE_CAP_W < 0) {
        for (uint8_t i = 1; i < pipeCount; ++i) {
          pipes[i - 1] = pipes[i];
        }
        --pipeCount;
      }
      if (pipeCount < MAX_PIPES &&
          (pipeCount == 0 ||
           pipes[pipeCount - 1].x <= screenW + PIPE_SPAWN_MARGIN - pipeSpacing)) {
        pipes[pipeCount++] = {
            screenW + static_cast<float>(PIPE_SPAWN_MARGIN),
            static_cast<int16_t>(
                random(35, screenH - GROUND_HEIGHT - GAP_HEIGHT - 20)),
            false};
      }

      const float bodyLeft = BIRD_X + BIRD_BODY_OFFSET_X - BIRD_BODY_HALF_W;
      const float bodyRight = BIRD_X + BIRD_BODY_OFFSET_X + BIRD_BODY_HALF_W;
      const float bodyTop = birdY + BIRD_BODY_OFFSET_Y - BIRD_BODY_HALF_H;
      const float bodyBottom = birdY + BIRD_BODY_OFFSET_Y + BIRD_BODY_HALF_H;
      bool hitPipe = false;

      for (uint8_t i = 0; i < pipeCount; ++i) {
        Pipe& pipe = pipes[i];
        if (!pipe.scored && pipe.x + PIPE_WIDTH < BIRD_X) {
          ++score;
          pipeSpeed = min(pipeSpeed + PIPE_SPEED_STEP, PIPE_SPEED_MAX);
          pipe.scored = true;
          playScoreSound();
        }

        const int16_t gapBottom = pipe.gapTop + GAP_HEIGHT;
        const bool inPipeX =
            bodyRight > pipe.x && bodyLeft < pipe.x + PIPE_WIDTH;
        // The cap sticks out past the stem, so it is checked over its own rows.
        const bool inCapX = bodyRight > pipe.x - PIPE_CAP_OVERHANG &&
                            bodyLeft < pipe.x + PIPE_WIDTH + PIPE_CAP_OVERHANG;
        const bool hitCap =
            inCapX &&
            ((bodyTop < pipe.gapTop && bodyBottom > pipe.gapTop - PIPE_CAP_H) ||
             (bodyBottom > gapBottom && bodyTop < gapBottom + PIPE_CAP_H));
        if (hitCap ||
            (inPipeX && (bodyTop < pipe.gapTop || bodyBottom > gapBottom))) {
          hitPipe = true;
        }
      }

      const bool outsideScreen =
          bodyTop < 0 || bodyBottom >= screenH - GROUND_HEIGHT;
      if (outsideScreen || hitPipe) {
        break;
      }

      drawBackground(display, sprite, night);
      if (useSprite) {
        drawFrame(gameFrame, birdY, birdVelocity, pipeSpeed, pipes, pipeCount,
                  score, bestAtStart, flapStartedAt);
        gameFrame.pushSprite(0, 0);
      } else {
        drawFrame(display, birdY, birdVelocity, pipeSpeed, pipes, pipeCount,
                  score, bestAtStart, flapStartedAt);
      }
    }

    const bool newBest = leaderboardRecordScore("flappy", score);
    const uint32_t best = leaderboardBest("flappy");
    const uint32_t totalFlaps = leaderboardAddFlaps("flappy", roundFlaps);
    uint32_t top[LEADERBOARD_SIZE];
    const uint8_t topCount = leaderboardTop("flappy", top, LEADERBOARD_SIZE);
    drawBackground(display, sprite, night);
    if (useSprite) {
      drawGameOver(gameFrame, score, best, newBest, top, topCount, totalFlaps);
      gameFrame.pushSprite(0, 0);
    } else {
      drawGameOver(display, score, best, newBest, top, topCount, totalFlaps);
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

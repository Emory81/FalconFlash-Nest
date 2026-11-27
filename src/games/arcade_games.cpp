#include "games/arcade_games.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "audio/audio_helper.h"
#include "game_assets.h"
#include "games/checkers_rules.h"
#include "games/chess_rules.h"
#include "golf_levels.h"
#include "input/button_input.h"
#include "super_flashy_art.h"
#include "super_flashy_birds.h"

// Controls (only one button can be read at a time):
//   Tic Tac Toe:          BTN1 left (hold: place), BTN2 up, BTN3 down,
//                         BTN4 right
//   Checkers, Chess:      as Tic Tac Toe (hold BTN1: pick up / move)
//   Rock Paper Scissors:  BTN1 rock, BTN2 paper, BTN3 scissors, BTN4 day/night
//   Brick Breaker:        BTN1 left, BTN4 right, BTN3 launch
//   Runner:               BTN3 jump, hold BTN2 to duck, BTN1/4 edge back and
//                         forth a little
//   Pong:                 BTN1 up, BTN2 down, BTN3 serve
//   Flashy Golf:          BTN1 flap left, BTN4 flap right, BTN2 restart,
//                         tap BTN3 for the level screen
// Two players:
//   Tic Tac Toe:          take turns with the one-player buttons
//   Checkers, Chess:      take turns with the one-player buttons
//   Rock Paper Scissors:  pick one after the other (the other looks away)
//   Pong:                 tap BTN1 (left) or BTN4 (right) to turn your paddle
// Holding BTN3 for 3 seconds exits all but Rock Paper Scissors (hold BTN4).
namespace {
constexpr int16_t W = 320;
constexpr int16_t H = 240;
constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t EXIT_HOLD_MS = 3000;
constexpr uint32_t EXIT_SHOW_MS = 800;
constexpr uint32_t PLACE_HOLD_MS = 600;  // Tic Tac Toe: hold BTN1 to place

struct Input {
  Button pressed;  // pressed since the last frame
  Button held;
  bool tap;  // the exit button (usually BTN3) let go after a short press
};

// Frame pacing, button sampling, double buffering and hold-to-exit.
class Session {
 public:
  // bufferRows < H draws the screen in strips with showStrips(), for games
  // that need most of the memory for something else.
  explicit Session(TFT_eSPI& screen, Button exitWith = Button::Button3,
                   int16_t bufferRows = H)
      : display(screen), frame(&screen), exitButton(exitWith),
        rows(bufferRows) {
    waitForButtonRelease();
    audioBegin();
    frame.setColorDepth(8);
    frame.createSprite(W, bufferRows);
  }

  ~Session() {
    audioEnd();
    frame.deleteSprite();
    waitForButtonRelease();
  }

  // The frame buffer's pixels (RGB332), or null when there is none.
  uint8_t* buffer() {
    return frame.created() ? static_cast<uint8_t*>(frame.getPointer()) : nullptr;
  }

  void releaseBuffer() { frame.deleteSprite(); }

  // Try again after a large image has been freed.
  void reserveBuffer() {
    if (frame.created()) return;
    frame.setColorDepth(8);
    frame.createSprite(W, rows);
    if (!frame.created() && rows > 80) {
      rows = 80;
      frame.createSprite(W, rows);
    }
  }

  bool fullBuffer() { return frame.created() && rows >= H; }

  // One push when the buffer is the whole screen. Shorter buffers are pushed
  // in bands so the picture is never drawn straight onto the panel.
  template <typename Scene>
  void present(const Scene& scene) {
    if (!frame.created()) {
      scene(display);
      drawExitBar(display);
      return;
    }
    if (rows >= H) {
      scene(frame);
      drawExitBar(frame);
      frame.pushSprite(0, 0);
      return;
    }
    for (int16_t top = 0; top < H; top += rows) {
      frame.resetViewport();
      frame.fillSprite(TFT_BLACK);
      frame.setViewport(0, -top, W, H, true);
      scene(frame);
      drawExitBar(frame);
      frame.resetViewport();
      frame.pushSprite(0, top);
    }
  }

  // Where to draw this frame.
  TFT_eSPI& canvas() {
    return frame.created() ? static_cast<TFT_eSPI&>(frame) : display;
  }

  // Draws one frame a strip at a time: the background (W x H, RGB332, or a
  // plain color if null), then scene(canvas) in ordinary screen coordinates.
  template <typename Scene>
  void showStrips(const uint8_t* background, uint16_t plain,
                  const Scene& scene) {
    if (!frame.created()) {  // no memory even for a strip
      if (background != nullptr) {
        display.pushImage(0, 0, W, H, const_cast<uint8_t*>(background), true);
      } else {
        display.fillRect(0, 0, W, H, plain);
      }
      scene(display);
      drawExitBar(display);
      return;
    }
    for (int16_t top = 0; top < H; top += rows) {
      frame.resetViewport();
      if (background != nullptr) {
        memcpy(frame.getPointer(), background + top * W, W * rows);
      } else {
        frame.fillSprite(plain);
      }
      // Shift the drawing up so this strip's part lands in the buffer.
      frame.setViewport(0, -top, W, H, true);
      scene(frame);
      drawExitBar(frame);
      frame.resetViewport();
      frame.pushSprite(0, top);
    }
  }

  // Waits for the next frame. False once the player has held BTN3 to exit.
  bool next(Input& in) {
    Button pressed = Button::None;
    while (true) {
      const Button event = readButtonEvent();
      if (event != Button::None) pressed = event;
      if (millis() - lastFrame >= FRAME_MS) break;
      delay(2);
    }
    const uint32_t now = millis();
    lastFrame = now;
    in.pressed = pressed;
    in.held = readButton();
    in.tap = false;
    if (in.held == exitButton) {
      if (since == 0) since = now;
      heldMs = now - since;
      if (heldMs >= EXIT_HOLD_MS) return false;
    } else {
      // The second case is a tap shorter than a frame.
      in.tap = since != 0 ? heldMs < EXIT_SHOW_MS : pressed == exitButton;
      since = 0;
      heldMs = 0;
    }
    return true;
  }

  // How far the exit bar has got, 0 while it is not showing.
  uint32_t exitBar() const { return heldMs < EXIT_SHOW_MS ? 0 : heldMs; }

  // Shows the frame, with the exit bar on top while BTN3 is held.
  void show() {
    drawExitBar(canvas());
    if (frame.created()) frame.pushSprite(0, 0);
  }

 private:
  void drawExitBar(TFT_eSPI& d) {
    if (heldMs < EXIT_SHOW_MS) return;
    d.fillRoundRect(20, 8, W - 40, 22, 4, TFT_BLACK);
    d.drawRoundRect(20, 8, W - 40, 22, 4, TFT_WHITE);
    d.fillRect(24, 22, (W - 48) * heldMs / EXIT_HOLD_MS, 4, TFT_YELLOW);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TC_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(exitButton == Button::Button4 ? "Keep holding BTN4 to exit"
                                               : "Keep holding BTN3 to exit",
                 W / 2, 12);
  }

  TFT_eSPI& display;
  TFT_eSprite frame;
  Button exitButton;
  int16_t rows;  // height of the frame buffer
  uint32_t lastFrame = 0;
  uint32_t since = 0;
  uint32_t heldMs = 0;
};

void text(TFT_eSPI& d, const char* s, int16_t x, int16_t y, uint16_t color,
          uint8_t size = 1, uint8_t datum = MC_DATUM) {
  d.setTextFont(1);
  d.setTextSize(size);
  d.setTextDatum(datum);
  d.setTextColor(color);
  d.drawString(s, x, y);
}

void banner(TFT_eSPI& d, const char* title, const char* hint) {
  d.fillRoundRect(40, 90, W - 80, 56, 6, TFT_BLACK);
  d.drawRoundRect(40, 90, W - 80, 56, 6, TFT_YELLOW);
  text(d, title, W / 2, 108, TFT_YELLOW, 2);
  text(d, hint, W / 2, 132, TFT_WHITE);
}

// ---- Tic Tac Toe ------------------------------------------------------------
constexpr int8_t LINES[8][3] = {{0, 1, 2}, {3, 4, 5}, {6, 7, 8}, {0, 3, 6},
                                {1, 4, 7}, {2, 5, 8}, {0, 4, 8}, {2, 4, 6}};

// 1 = player (X) has a line, 2 = the computer (O), 3 = full board, 0 = neither.
uint8_t boardResult(const uint8_t* cells) {
  for (const auto& l : LINES) {
    if (cells[l[0]] != 0 && cells[l[0]] == cells[l[1]] &&
        cells[l[1]] == cells[l[2]]) {
      return cells[l[0]];
    }
  }
  for (uint8_t i = 0; i < 9; ++i) {
    if (cells[i] == 0) return 0;
  }
  return 3;
}

// The empty cell that completes a line for `who`, or -1.
int8_t finishingCell(const uint8_t* cells, uint8_t who) {
  for (const auto& l : LINES) {
    uint8_t mine = 0;
    int8_t gap = -1;
    for (const int8_t i : l) {
      if (cells[i] == who) ++mine;
      if (cells[i] == 0) gap = i;
    }
    if (mine == 2 && gap >= 0) return gap;
  }
  return -1;
}

int8_t computerMove(const uint8_t* cells) {
  // Now and then it plays carelessly, so it can be beaten.
  if (random(4) != 0) {
    int8_t i = finishingCell(cells, 2);
    if (i >= 0) return i;
    i = finishingCell(cells, 1);
    if (i >= 0) return i;
    if (cells[4] == 0) return 4;
  }
  int8_t open[9];
  uint8_t count = 0;
  for (int8_t i = 0; i < 9; ++i) {
    if (cells[i] == 0) open[count++] = i;
  }
  return count == 0 ? -1 : open[random(count)];
}

// ---- Checkers ---------------------------------------------------------------
// Checkers and chess draw in strips this tall: a whole-screen buffer does not
// always fit, and drawing straight to the screen flashes.
constexpr int16_t BOARD_ROWS = 40;

// Add bytes to a running hash (FNV-1a).
uint32_t mix(uint32_t hash, const void* data, size_t size) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

constexpr int16_t SQUARE = 28;
constexpr int16_t BOARD_LEFT = (W - SQUARE * 8) / 2;
constexpr int16_t BOARD_TOP = (H - SQUARE * 8) / 2;
constexpr uint8_t CHECKERS_DEPTH = 4;  // turns the computer looks ahead
constexpr uint16_t LIGHT_SQUARE = 0xEF18;
constexpr uint16_t DARK_SQUARE = 0x7A87;

void drawPiece(TFT_eSPI& d, uint8_t piece, int16_t x, int16_t y, int16_t r) {
  const bool first = checkers::sideOf(piece) == checkers::FIRST;
  d.fillCircle(x, y, r, first ? TFT_RED : TFT_BLACK);
  d.drawCircle(x, y, r, first ? 0xFD14 : TFT_LIGHTGREY);
  d.drawCircle(x, y, r - 3, first ? 0xA000 : TFT_DARKGREY);
  if (piece & checkers::KING) {  // a small crown
    d.fillRect(x - 5, y, 11, 4, TFT_YELLOW);
    d.fillTriangle(x - 5, y, x - 5, y - 5, x - 1, y, TFT_YELLOW);
    d.fillTriangle(x - 3, y, x, y - 6, x + 3, y, TFT_YELLOW);
    d.fillTriangle(x + 1, y, x + 5, y - 5, x + 5, y, TFT_YELLOW);
  }
}

// ---- Chess ------------------------------------------------------------------
constexpr uint8_t CHESS_DEPTH = 3;  // half-moves the computer looks ahead

// Pawn, knight, bishop, rook, queen, king: 12 x 12, drawn double size.
const char* const CHESS_ART[6][12] = {
    {"............", "............", ".....##.....", "....####....",
     "....####....", ".....##.....", "....####....", ".....##.....",
     "....####....", "...######...", "..########..", "............"},
    {"............", ".....#.#....", "....#####...", "...##.####..",
     "..########..", "..###.####..", "......####..", ".....####...",
     "....#####...", "...######...", "..########..", "............"},
    {"............", ".....##.....", "....##.#....", "...##.###...",
     "...#.####...", "...######...", "....####....", ".....##.....",
     "....####....", "...######...", "..########..", "............"},
    {"............", "..##.##.##..", "..##.##.##..", "..########..",
     "...######...", "....####....", "....####....", "....####....",
     "...######...", "..########..", "..########..", "............"},
    {".....##.....", ".#...##...#.", ".#..####..#.", ".##.####.##.",
     ".##########.", "..########..", "..########..", "...######...",
     "...######...", "..########..", "..########..", "............"},
    {".....##.....", "...######...", ".....##.....", "..##.##.##..",
     ".##########.", ".##########.", "..########..", "...######...",
     "...######...", "..########..", "..########..", "............"},
};

// A piece with its top-left at (x, y), 24 x 24, outlined in the other color.
void drawChessPiece(TFT_eSPI& d, uint8_t piece, int16_t x, int16_t y) {
  const bool white = chess::sideOf(piece) == chess::WHITE;
  const char* const* art = CHESS_ART[chess::typeOf(piece) - 1];
  const auto filled = [&](int8_t r, int8_t c) {
    return r >= 0 && r < 12 && c >= 0 && c < 12 && art[r][c] == '#';
  };
  const uint16_t body = white ? TFT_WHITE : TFT_BLACK;
  const uint16_t edge = white ? TFT_BLACK : TFT_LIGHTGREY;
  // The outline first, one pixel around every part of the shape.
  for (int8_t r = 0; r < 12; ++r) {
    for (int8_t c = 0; c < 12; ++c) {
      if (filled(r, c)) d.fillRect(x + c * 2 - 1, y + r * 2 - 1, 4, 4, edge);
    }
  }
  for (int8_t r = 0; r < 12; ++r) {
    for (int8_t c = 0; c < 12; ++c) {
      if (filled(r, c)) d.fillRect(x + c * 2, y + r * 2, 2, 2, body);
    }
  }
}

// ---- Rock Paper Scissors ----------------------------------------------------
const char* const HAND_NAMES[] = {"Rock", "Paper", "Scissors"};

// A hand sign centered on (x, y), about 40 px across.
void drawHand(TFT_eSPI& d, uint8_t hand, int16_t x, int16_t y) {
  if (hand == 0) {
    d.fillCircle(x, y + 2, 17, TFT_DARKGREY);
    d.fillCircle(x - 7, y - 6, 10, TFT_LIGHTGREY);
    d.drawCircle(x, y + 2, 17, TFT_WHITE);
  } else if (hand == 1) {
    d.fillRect(x - 15, y - 19, 30, 38, TFT_WHITE);
    for (int16_t line = -11; line <= 11; line += 7) {
      d.drawFastHLine(x - 10, y + line, 20, TFT_BLUE);
    }
  } else {
    for (int8_t o = -1; o <= 1; ++o) {
      d.drawLine(x - 12 + o, y - 19, x + 8 + o, y + 9, TFT_LIGHTGREY);
      d.drawLine(x + 12 + o, y - 19, x - 8 + o, y + 9, TFT_LIGHTGREY);
    }
    d.drawCircle(x - 10, y + 14, 6, TFT_RED);
    d.drawCircle(x - 10, y + 14, 5, TFT_RED);
    d.drawCircle(x + 10, y + 14, 6, TFT_RED);
    d.drawCircle(x + 10, y + 14, 5, TFT_RED);
  }
}

// ---- Brick Breaker ----------------------------------------------------------
constexpr uint8_t BRICK_COLS = 10;
constexpr uint8_t BRICK_ROWS = 5;
constexpr int16_t BRICK_W = 30;
constexpr int16_t BRICK_H = 10;
constexpr int16_t BRICK_GAP = 2;
constexpr int16_t BRICK_LEFT = 1;
constexpr int16_t BRICK_TOP = 28;
constexpr int16_t PADDLE_W = 48;
constexpr int16_t PADDLE_Y = 224;
constexpr float PADDLE_SPEED = 10.0f;

// ---- Runner -----------------------------------------------------------------
constexpr int16_t GROUND_Y = 200;
constexpr int16_t RUNNER_X = 36;        // where the bird starts
constexpr float RUNNER_REACH = 18.0f;   // how far it can edge from there
constexpr float RUNNER_STEP = 1.5f;
constexpr float RUNNER_JUMP = -9.5f;
constexpr float RUNNER_GRAVITY = 0.7f;
constexpr float RUNNER_START_SPEED = 4.0f;  // px per frame
constexpr float RUNNER_TOP_SPEED = 11.0f;
constexpr float RUNNER_SPEED_UP = 0.006f;   // added every frame

struct Obstacle {
  bool active;
  bool flying;  // duck under (or jump over) it
  float x;
  int16_t w;
  int16_t h;
  bool red;  // red gorillas, for variety
};

// A w x h sprite with its top-left at (x, y), stretched to destW x destH.
void drawArt(TFT_eSPI& d, int16_t x, int16_t y, int16_t w, int16_t h,
             const uint16_t* pixels, bool flip = false, int16_t destW = 0,
             int16_t destH = 0) {
  if (destW == 0) destW = w;
  if (destH == 0) destH = h;
  for (int16_t dy = 0; dy < destH; ++dy) {
    for (int16_t dx = 0; dx < destW; ++dx) {
      const int16_t sx = dx * w / destW;
      if (pixels == nullptr) return;
      const uint16_t c = pixels[(dy * h / destH) * w + (flip ? w - 1 - sx : sx)];
      if (c != SFB_TRANSPARENT) d.drawPixel(x + dx, y + dy, c);
    }
  }
}

// ---- The egg ball -----------------------------------------------------------
// The golden egg is an oval: half as wide as EGG_A * 2, tall as EGG_B * 2.
constexpr float EGG_A = 6.0f;
constexpr float EGG_B = 8.0f;

// How far a tilted egg reaches sideways and up or down from its middle.
float eggReachX(float angle) {
  return hypotf(EGG_A * cosf(angle), EGG_B * sinf(angle));
}
float eggReachY(float angle) {
  return hypotf(EGG_A * sinf(angle), EGG_B * cosf(angle));
}

// The velocity has just been mirrored off a surface; being oval and spinning,
// the egg then kicks off at an odd angle. `side` tells which way the surface
// faced; `amount` scales the oddness.
void eggKick(float& vx, float& vy, float& spin, float angle, float speed,
             bool side, float amount) {
  const float awayX = vx < 0 ? -1.0f : 1.0f;
  const float awayY = vy < 0 ? -1.0f : 1.0f;
  const float turn =
      amount * constrain(0.35f * sinf(2 * angle) + 0.8f * spin, -0.5f, 0.5f);
  const float nx = vx * cosf(turn) - vy * sinf(turn);
  const float ny = vx * sinf(turn) + vy * cosf(turn);
  vx = nx;
  vy = ny;
  // Still head away from what was hit, and never skim almost level.
  if (side) {
    vx = awayX * fabsf(vx);
  } else {
    vy = awayY * fabsf(vy);
  }
  const float scale = speed / max(0.1f, hypotf(vx, vy));
  vx *= scale;
  vy *= scale;
  if (fabsf(vy) < speed * 0.4f) {
    vy = (vy < 0 ? -1.0f : 1.0f) * speed * 0.4f;
    vx = (vx < 0 ? -1.0f : 1.0f) * sqrtf(speed * speed - vy * vy);
  }
  // Glancing hits set it tumbling.
  spin = constrain((side ? vy : vx) * 0.05f + random(-10, 11) / 100.0f, -0.35f,
                   0.35f);
}

// The golden egg turned by `angle`, centered on (cx, cy).
void drawSpunEgg(TFT_eSPI& d, float cx, float cy, float angle) {
  if (sfbGoldEgg == nullptr || sfbGoldEggWidth == 0 || sfbGoldEggHeight == 0) {
    d.fillEllipse(lroundf(cx), lroundf(cy), lroundf(EGG_A), lroundf(EGG_B),
                  TFT_YELLOW);
    return;
  }
  const float c = cosf(angle);
  const float s = sinf(angle);
  constexpr int16_t R = 10;
  for (int16_t dy = -R; dy <= R; ++dy) {
    for (int16_t dx = -R; dx <= R; ++dx) {
      const int16_t sx = static_cast<int16_t>(floorf(c * dx + s * dy + 8.0f));
      const int16_t sy = static_cast<int16_t>(floorf(-s * dx + c * dy + 8.0f));
      if (sx < 0 || sx >= sfbGoldEggWidth || sy < 0 || sy >= sfbGoldEggHeight) {
        continue;
      }
      const uint16_t color =
          pgm_read_word(&sfbGoldEgg[sy * sfbGoldEggWidth + sx]);
      if (color != SFB_TRANSPARENT) {
        d.drawPixel(lroundf(cx) + dx, lroundf(cy) + dy, color);
      }
    }
  }
}

// Red gorillas shuffle from side to side as they come: how far from o.x one is
// right now. The others stay put.
float sway(const Obstacle& o) {
  return o.red && !o.flying ? 14.0f * sinf(o.x / 24.0f) : 0.0f;
}

// The best Runner score is kept in flash, so it survives powering off.
uint32_t loadRunnerBest() {
  Preferences preferences;
  preferences.begin("runner", true);
  const uint32_t best = preferences.getUInt("best", 0);
  preferences.end();
  return best;
}

void saveRunnerBest(uint32_t best) {
  Preferences preferences;
  preferences.begin("runner", false);
  preferences.putUInt("best", best);
  preferences.end();
}

// ---- Flashy Golf ------------------------------------------------------------
// Side-on golf: the golf egg has wings, and each press flaps it up and to one
// side. Fewest flaps into the hole wins. Maps are in golf/.
// The egg's solid radius: the egg in its picture is 14 px wide, and the wings
// either side of it hit nothing.
constexpr float GOLF_R = 7.0f;
constexpr int16_t GOLF_ART_W = 20;
constexpr float GOLF_GRAVITY = 0.26f;
constexpr float GOLF_MAX_FALL = 7.0f;
constexpr float GOLF_MAX_RISE = 4.8f;
constexpr float GOLF_MAX_SPEED = 6.0f;
constexpr float GOLF_DRAG = 0.995f;      // sideways, each frame in the air
constexpr float GOLF_ROLL_DRAG = 0.985f;  // and on the ground: it slides far
// A flap is a kick followed by a few frames of push while the wings are down.
// Flaps add up: more of them go faster and higher.
constexpr float GOLF_FLAP_UP = 3.7f;
constexpr float GOLF_KEEP_RISE = 0.25f;  // of the climb it already had
constexpr float GOLF_FLAP_PUSH = 0.9f;  // from standing
// Added for each unit of speed it already has that way, so it builds up.
constexpr float GOLF_BUILD = 0.3f;
constexpr float GOLF_TURN_KEEP = 0.35f;  // of the speed the other way
constexpr uint8_t GOLF_THRUST_FRAMES = 6;
constexpr float GOLF_THRUST_UP = 0.08f;
constexpr float GOLF_THRUST_ACROSS = 0.04f;
constexpr uint8_t GOLF_STEPS = 3;  // collision checks per frame
constexpr int16_t GOLF_TILE = 16;
constexpr uint16_t GOLF_GOLD = 0xFEA0;
constexpr uint16_t GOLF_SILVER = 0xC618;

struct GolfBall {
  float x, y, vx, vy;
  uint8_t thrust;  // frames of push left from the last flap
  int8_t side;     // -1 or 1, the way that flap went
};

void golfFlap(GolfBall& b, int8_t side) {
  b.vy = max(min(b.vy, 0.0f) * GOLF_KEEP_RISE - GOLF_FLAP_UP, -GOLF_MAX_RISE);
  if (b.vx * side < 0) b.vx *= GOLF_TURN_KEEP;
  const float push = GOLF_FLAP_PUSH + GOLF_BUILD * max(b.vx * side, 0.0f);
  b.vx = constrain(b.vx + side * push, -GOLF_MAX_SPEED, GOLF_MAX_SPEED);
  b.thrust = GOLF_THRUST_FRAMES;
  b.side = side;
}

struct GolfHole {
  int16_t x, y;  // top-left of the two tiles the cup is set into
};

char golfAt(const GolfLevel& level, int16_t col, int16_t row) {
  if (col < 0 || col >= level.columns || row < 0 || row >= GOLF_ROWS) return '.';
  return level.rows[row][col];
}

// Push the egg out of one solid rectangle, bouncing it. True if it is now
// being held up by it.
bool golfHit(GolfBall& b, float rx, float ry, float rw, float rh) {
  const float dx = b.x - constrain(b.x, rx, rx + rw);
  const float dy = b.y - constrain(b.y, ry, ry + rh);
  const float d2 = dx * dx + dy * dy;
  if (d2 >= GOLF_R * GOLF_R) return false;
  const float d = sqrtf(d2);
  const float nx = d > 0.001f ? dx / d : 0.0f;
  const float ny = d > 0.001f ? dy / d : -1.0f;
  b.x += nx * (GOLF_R - d);
  b.y += ny * (GOLF_R - d);
  if (ny < -0.7f && b.vy >= 0) b.thrust = 0;
  const float vn = b.vx * nx + b.vy * ny;
  if (vn >= 0) return ny < -0.7f;
  const float tx = b.vx - vn * nx, ty = b.vy - vn * ny;
  if (-vn > 1.2f) {  // a bounce
    // The ground deadens a fall much more than a wall turns the egg back.
    const float spring = ny < -0.7f ? 0.28f : 0.45f;
    b.vx = tx * 0.92f - spring * vn * nx;
    b.vy = ty * 0.92f - spring * vn * ny;
  } else {  // rolling: golfFrame slows it, once a frame
    b.vx = tx;
    b.vy = ty;
  }
  return ny < -0.7f;
}

// One frame of flight; true if something is under the egg.
bool golfFrame(GolfBall& b, const GolfLevel& level, const GolfHole& hole) {
  const float width = level.columns * GOLF_TILE;
  bool grounded = false;
  if (b.thrust > 0) {
    --b.thrust;
    b.vy -= GOLF_THRUST_UP;
    b.vx += b.side * GOLF_THRUST_ACROSS;
  }
  b.vx = constrain(b.vx * GOLF_DRAG, -GOLF_MAX_SPEED, GOLF_MAX_SPEED);
  b.vy = constrain(b.vy + GOLF_GRAVITY, -GOLF_MAX_RISE, GOLF_MAX_FALL);
  for (uint8_t step = 0; step < GOLF_STEPS; ++step) {
    b.x += b.vx / GOLF_STEPS;
    b.y += b.vy / GOLF_STEPS;
    if (b.x < GOLF_R) {
      b.x = GOLF_R;
      b.vx = fabsf(b.vx) * 0.5f;
    }
    if (b.x > width - GOLF_R) {
      b.x = width - GOLF_R;
      b.vx = -fabsf(b.vx) * 0.5f;
    }
    const int16_t col = static_cast<int16_t>(b.x) / GOLF_TILE;
    const int16_t row = static_cast<int16_t>(floorf(b.y / GOLF_TILE));
    for (int16_t r = row - 1; r <= row + 1; ++r) {
      for (int16_t c = col - 1; c <= col + 1; ++c) {
        const char tile = golfAt(level, c, r);
        if (tile == 'X' || tile == 'H' || tile == 'B' || tile == 'C') {
          grounded |=
              golfHit(b, c * GOLF_TILE, r * GOLF_TILE, GOLF_TILE, GOLF_TILE);
        }
      }
    }
    // The cup: a lip each side and a floor.
    grounded |= golfHit(b, hole.x, hole.y, 4, 16);
    grounded |= golfHit(b, hole.x + 28, hole.y, 4, 16);
    grounded |= golfHit(b, hole.x, hole.y + 14, 32, 2);
  }
  if (grounded) b.vx *= GOLF_ROLL_DRAG;
  return grounded;
}

// The golf egg turned by `angle` about its middle, at (cx, cy).
void drawGolfEgg(TFT_eSPI& d, int16_t cx, int16_t cy, float angle,
                 const uint16_t* art) {
  const float c = cosf(angle), s = sinf(angle);
  constexpr int16_t R = 13;  // reaches the corners of the 20 x 16 picture
  for (int16_t dy = -R; dy <= R; ++dy) {
    for (int16_t dx = -R; dx <= R; ++dx) {
      const int16_t sx = static_cast<int16_t>(floorf(c * dx + s * dy + GOLF_ART_W / 2.0f));
      const int16_t sy = static_cast<int16_t>(floorf(-s * dx + c * dy + 8.0f));
      if (sx < 0 || sx >= GOLF_ART_W || sy < 0 || sy >= 16) continue;
      const uint16_t color = pgm_read_word(&art[sy * GOLF_ART_W + sx]);
      if (color != SFB_TRANSPARENT) d.drawPixel(cx + dx, cy + dy, color);
    }
  }
}

void golfStar(TFT_eSPI& d, int16_t x, int16_t y, int16_t r, uint16_t color,
              bool filled) {
  int16_t px[10], py[10];
  for (uint8_t i = 0; i < 10; ++i) {
    const float angle = i * 3.14159f / 5 - 3.14159f / 2;
    const float reach = i % 2 == 0 ? r : r * 0.45f;
    px[i] = x + lroundf(cosf(angle) * reach);
    py[i] = y + lroundf(sinf(angle) * reach);
  }
  for (uint8_t i = 0; i < 10; ++i) {
    const uint8_t j = (i + 1) % 10;
    if (filled) d.fillTriangle(x, y, px[i], py[i], px[j], py[j], color);
    d.drawLine(px[i], py[i], px[j], py[j], filled ? TFT_BLACK : color);
  }
}

// The fewest flaps each hole has been finished in, 0 if never; kept in flash.
uint16_t loadGolfBest(uint8_t level) {
  char key[8];
  snprintf(key, sizeof(key), "best%u", level);
  Preferences preferences;
  preferences.begin("golf", true);
  const uint16_t best = preferences.getUShort(key, 0);
  preferences.end();
  return best;
}

void saveGolfBest(uint8_t level, uint16_t best) {
  char key[8];
  snprintf(key, sizeof(key), "best%u", level);
  Preferences preferences;
  preferences.begin("golf", false);
  preferences.putUShort(key, best);
  preferences.end();
}

void drawGolfSelect(TFT_eSPI& d, uint8_t chosen, const uint16_t* best) {
  d.fillRect(0, 0, W, H, 0x5DFF);
  for (int16_t x = 0; x < W; x += 16) {
    drawArt(d, x, H - 32, 16, 16, sfbGrass);
    drawArt(d, x, H - 16, 16, 16, sfbDirt);
  }
  text(d, "FLASHY GOLF", W / 2, 22, TFT_BLACK, 3);
  for (uint8_t i = 0; i < GOLF_COUNT; ++i) {
    const GolfLevel& level = GOLF_LEVELS[i];
    const int16_t x = 10 + i * 104, y = 48, mid = x + 46;
    d.fillRoundRect(x, y, 92, 132, 8, TFT_BLACK);
    d.drawRoundRect(x, y, 92, 132, 8, i == chosen ? TFT_YELLOW : TFT_WHITE);
    if (i == chosen) d.drawRoundRect(x + 2, y + 2, 88, 128, 6, TFT_YELLOW);
    char line[24];
    snprintf(line, sizeof(line), "Level %u", i + 1);
    text(d, line, mid, y + 16, TFT_WHITE, 1);
    const bool gold = best[i] != 0 && best[i] <= level.gold;
    const bool silver = best[i] != 0 && best[i] <= level.silver;
    golfStar(d, mid - 22, y + 46, 17, GOLF_GOLD, gold);
    golfStar(d, mid + 22, y + 46, 17, GOLF_SILVER, silver);
    snprintf(line, sizeof(line), "%u", level.gold);
    text(d, line, mid - 22, y + 72, GOLF_GOLD);
    snprintf(line, sizeof(line), "%u", level.silver);
    text(d, line, mid + 22, y + 72, GOLF_SILVER);
    text(d, "flaps for par", mid, y + 88, TFT_LIGHTGREY);
    if (best[i] == 0) {
      snprintf(line, sizeof(line), "Best --");
    } else {
      snprintf(line, sizeof(line), "Best %u", best[i]);
    }
    text(d, line, mid, y + 112, TFT_WHITE);
  }
  text(d, "1/4 choose   3 play   hold 3 exit", W / 2, 194, TFT_BLACK);
}

// ---- Pong -------------------------------------------------------------------
constexpr int16_t PONG_PADDLE_H = 40;
constexpr int16_t PONG_PADDLE_W = 6;
constexpr int16_t PONG_LEFT = 8;
constexpr int16_t PONG_RIGHT = W - 8 - PONG_PADDLE_W;
constexpr int16_t PONG_TOP = 20;
constexpr float PONG_PLAYER_SPEED = 5.0f;
constexpr float PONG_CPU_SPEED = 2.9f;
constexpr float PONG_TAP_SPEED = 3.5f;  // two-player paddles
constexpr uint8_t PONG_WIN = 7;
}  // namespace

void runTicTacToe(TFT_eSPI& display, bool twoPlayer) {
  Session session(display);
  uint8_t cells[9] = {0};
  uint8_t cursor = 4;
  uint8_t result = 0;
  uint16_t wins = 0, losses = 0, draws = 0;
  bool playerStarts = true;
  bool playerTurn = true;
  uint32_t computerAt = 0;
  uint32_t since1 = 0;  // BTN1 went down: a tap moves, a hold places
  Input in;

  const auto finishTurn = [&]() {
    result = boardResult(cells);
    if (result == 1) ++wins;
    if (result == 2) ++losses;
    if (result == 3) ++draws;
    if (result != 0) {
      const bool cheer = result == 1 || (twoPlayer && result == 2);
      audioPlayTone(cheer ? 1320 : 220, 200);
    }
  };

  while (session.next(in)) {
    if (result != 0) {
      since1 = 0;
      // Any button starts the next round; the other side begins.
      if (in.pressed != Button::None) {
        for (auto& c : cells) c = 0;
        result = 0;
        playerStarts = !playerStarts;
        playerTurn = playerStarts;
        computerAt = millis() + 500;
      }
    } else {
      const uint8_t row = cursor / 3;
      const uint8_t col = cursor % 3;
      if (in.pressed == Button::Button1) since1 = millis();
      if (in.pressed == Button::Button4) cursor = row * 3 + (col + 1) % 3;
      if (in.pressed == Button::Button2) cursor = (cursor + 6) % 9;
      if (in.pressed == Button::Button3) cursor = (cursor + 3) % 9;

      bool place = false;
      if (since1 != 0) {
        if (in.held != Button::Button1) {  // a tap
          cursor = row * 3 + (col + 2) % 3;
          since1 = 0;
        } else if (millis() - since1 >= PLACE_HOLD_MS) {
          place = true;
          since1 = 0;
        }
      }
      if (place && (playerTurn || twoPlayer) && cells[cursor] == 0) {
        cells[cursor] = playerTurn ? 1 : 2;
        audioPlayTone(playerTurn ? 880 : 440, 30);
        playerTurn = !playerTurn;
        computerAt = millis() + 450;
        finishTurn();
      } else if (!twoPlayer && !playerTurn && millis() >= computerAt) {
        const int8_t move = computerMove(cells);
        if (move >= 0) cells[move] = 2;
        audioPlayTone(440, 30);
        playerTurn = true;
        finishTurn();
      }
    }

    TFT_eSPI& d = session.canvas();
    d.fillRect(0, 0, W, H, TFT_BLACK);
    char score[40];
    snprintf(score, sizeof(score),
             twoPlayer ? "X %u   O %u   Draw %u" : "You %u   CPU %u   Draw %u",
             wins, losses, draws);
    text(d, score, W / 2, 10, TFT_WHITE);
    if (twoPlayer && result == 0) {
      text(d, playerTurn ? "X's" : "O's", 35, 110,
           playerTurn ? TFT_CYAN : TFT_ORANGE, 2);
      text(d, "turn", 35, 132, TFT_WHITE, 2);
    }
    constexpr int16_t LEFT = 70, TOP = 30, CELL = 60;
    for (int8_t i = 1; i < 3; ++i) {
      d.fillRect(LEFT + i * CELL - 1, TOP, 3, CELL * 3, TFT_DARKGREY);
      d.fillRect(LEFT, TOP + i * CELL - 1, CELL * 3, 3, TFT_DARKGREY);
    }
    for (uint8_t i = 0; i < 9; ++i) {
      const int16_t cx = LEFT + (i % 3) * CELL + CELL / 2;
      const int16_t cy = TOP + (i / 3) * CELL + CELL / 2;
      if (cells[i] == 1) {
        for (int8_t o = -2; o <= 2; ++o) {
          d.drawLine(cx - 18 + o, cy - 18, cx + 18 + o, cy + 18, TFT_CYAN);
          d.drawLine(cx + 18 + o, cy - 18, cx - 18 + o, cy + 18, TFT_CYAN);
        }
      } else if (cells[i] == 2) {
        for (int8_t r = 17; r <= 20; ++r) d.drawCircle(cx, cy, r, TFT_ORANGE);
      }
      if (i == cursor && result == 0) {
        d.drawRect(cx - 26, cy - 26, 52, 52, TFT_YELLOW);
        d.drawRect(cx - 25, cy - 25, 50, 50, TFT_YELLOW);
      }
    }
    text(d, "1 left 2 up 3 down 4 right  hold 1: place", W / 2, H - 10, TFT_LIGHTGREY);
    if (result != 0) {
      const char* title = "Draw";
      if (result == 1) title = twoPlayer ? "X wins!" : "You win!";
      if (result == 2) title = twoPlayer ? "O wins!" : "CPU wins";
      banner(d, title, "Any button: play again");
    }
    session.show();
  }
}

void runCheckers(TFT_eSPI& display, bool twoPlayer) {
  Session session(display, Button::Button3, BOARD_ROWS);
  uint32_t shown = 0;  // what is on the screen now
  checkers::State state;
  checkers::reset(state);
  uint8_t cursor = 5 * 8;  // a piece of the player who starts
  int8_t selected = -1;    // the piece picked up
  int8_t movedTo = -1;     // where the last step ended
  uint8_t result = 0;
  uint16_t wins = 0, losses = 0;
  uint32_t computerAt = 0;
  uint32_t since1 = 0;  // BTN1 went down: a tap moves, a hold picks or places
  Input in;
  checkers::Move moves[checkers::MAX_MOVES];

  const auto step = [&](const checkers::Move& move) {
    checkers::apply(state, move);
    movedTo = move.to;
    audioPlayTone(move.taken >= 0 ? 1046 : 660, 30);
    selected = state.jumper;  // a jump that goes on keeps its piece
    result = checkers::result(state);
    if (result == checkers::FIRST) ++wins;
    if (result == checkers::SECOND) ++losses;
    if (result != 0) {
      const bool cheer = result == checkers::FIRST || (twoPlayer && result != 3);
      audioPlayTone(cheer ? 1320 : 220, 200);
    }
    computerAt = millis() + 500;
  };

  while (session.next(in)) {
    const bool computer = !twoPlayer && state.turn == checkers::SECOND;
    if (result != 0) {
      since1 = 0;
      if (in.pressed != Button::None) {  // any button starts a new game
        checkers::reset(state);
        result = 0;
        selected = -1;
        movedTo = -1;
        cursor = 5 * 8;
      }
    } else {
      const uint8_t row = cursor / 8;
      const uint8_t col = cursor % 8;
      if (in.pressed == Button::Button1) since1 = millis();
      if (in.pressed == Button::Button4) cursor = row * 8 + (col + 1) % 8;
      if (in.pressed == Button::Button2) cursor = (cursor + 56) % 64;
      if (in.pressed == Button::Button3) cursor = (cursor + 8) % 64;

      bool choose = false;
      if (since1 != 0) {
        if (in.held != Button::Button1) {  // a tap
          cursor = row * 8 + (col + 7) % 8;
          since1 = 0;
        } else if (millis() - since1 >= PLACE_HOLD_MS) {
          choose = true;
          since1 = 0;
        }
      }

      if (choose && !computer) {
        const uint8_t count = checkers::legalMoves(state, moves);
        bool done = false;
        for (uint8_t i = 0; i < count && !done; ++i) {
          if (moves[i].from == selected && moves[i].to == cursor) {
            step(moves[i]);
            done = true;
          }
        }
        if (!done && state.jumper < 0) {
          // Pick up a piece that can move; picking it again puts it down.
          bool movable = false;
          for (uint8_t i = 0; i < count; ++i) {
            if (moves[i].from == cursor) movable = true;
          }
          selected = movable && selected != cursor ? cursor : -1;
          audioPlayTone(movable ? 880 : 200, 30);
        }
      } else if (computer && millis() >= computerAt) {
        step(checkers::computerMove(
            state, CHECKERS_DEPTH,
            [](uint8_t n) { return static_cast<uint8_t>(random(n)); }));
      }
    }

    const uint8_t count = result == 0 ? checkers::legalMoves(state, moves) : 0;
    const bool mustJump = count > 0 && moves[0].taken >= 0;
    // Only draw when something has changed, so the screen holds still.
    uint32_t now = mix(2166136261u, &state, sizeof(state));
    const int32_t view[] = {cursor, selected, movedTo, result, wins, losses,
                            static_cast<int32_t>(session.exitBar())};
    now = mix(now, view, sizeof(view));
    if (now == shown) continue;
    shown = now;
    session.present([&](TFT_eSPI& d) {
      d.fillRect(0, 0, W, H, TFT_BLACK);
      uint8_t pieces[3] = {0, 0, 0};
      for (uint8_t i = 0; i < 64; ++i) {
        const int16_t x = BOARD_LEFT + (i % 8) * SQUARE;
        const int16_t y = BOARD_TOP + (i / 8) * SQUARE;
        d.fillRect(x, y, SQUARE, SQUARE,
                   (i / 8 + i % 8) % 2 == 0 ? LIGHT_SQUARE : DARK_SQUARE);
        if (i == movedTo) d.drawRect(x, y, SQUARE, SQUARE, TFT_CYAN);
        if (state.cells[i] != checkers::EMPTY) {
          ++pieces[checkers::sideOf(state.cells[i])];
          drawPiece(d, state.cells[i], x + SQUARE / 2, y + SQUARE / 2, 11);
        }
        if (i == selected) {
          d.drawRect(x + 1, y + 1, SQUARE - 2, SQUARE - 2, TFT_GREEN);
          d.drawRect(x + 2, y + 2, SQUARE - 4, SQUARE - 4, TFT_GREEN);
        }
      }
      // Where the picked-up piece may go.
      for (uint8_t i = 0; i < count; ++i) {
        if (moves[i].from != selected) continue;
        d.fillCircle(BOARD_LEFT + (moves[i].to % 8) * SQUARE + SQUARE / 2,
                     BOARD_TOP + (moves[i].to / 8) * SQUARE + SQUARE / 2, 4,
                     TFT_GREEN);
      }
      if (result == 0) {
        const int16_t x = BOARD_LEFT + (cursor % 8) * SQUARE;
        const int16_t y = BOARD_TOP + (cursor / 8) * SQUARE;
        d.drawRect(x, y, SQUARE, SQUARE, TFT_YELLOW);
        d.drawRect(x + 1, y + 1, SQUARE - 2, SQUARE - 2, TFT_YELLOW);
      }

      // Left of the board: whose turn and the controls. Right: pieces left.
      const bool firstTurn = state.turn == checkers::FIRST;
      text(d,
           twoPlayer ? (firstTurn ? "RED" : "BLK") : (firstTurn ? "YOUR" : "CPU"),
           BOARD_LEFT / 2, 20, firstTurn ? TFT_RED : TFT_WHITE);
      text(d, "turn", BOARD_LEFT / 2, 32, TFT_WHITE);
      if (mustJump) {
        text(d, "must", BOARD_LEFT / 2, 56, TFT_YELLOW);
        text(d, "jump", BOARD_LEFT / 2, 68, TFT_YELLOW);
      }
      const char* const help[] = {"1 left", "4 right", "2 up",
                                  "3 down", "hold 1",  "select"};
      for (uint8_t i = 0; i < 6; ++i) {
        text(d, help[i], BOARD_LEFT / 2, 140 + i * 13, TFT_LIGHTGREY);
      }
      const int16_t side = W - BOARD_LEFT / 2;
      char number[16];
      drawPiece(d, checkers::SECOND, side, 24, 9);
      snprintf(number, sizeof(number), "%u", pieces[checkers::SECOND]);
      text(d, number, side, 42, TFT_WHITE, 2);
      drawPiece(d, checkers::FIRST, side, H - 40, 9);
      snprintf(number, sizeof(number), "%u", pieces[checkers::FIRST]);
      text(d, number, side, H - 18, TFT_WHITE, 2);
      text(d, "wins", side, 96, TFT_LIGHTGREY);
      snprintf(number, sizeof(number), "%u-%u", wins, losses);
      text(d, number, side, 110, TFT_WHITE);

      if (result != 0) {
        const char* title = "Draw";
        if (result == checkers::FIRST) title = twoPlayer ? "Red wins!" : "You win!";
        if (result == checkers::SECOND) {
          title = twoPlayer ? "Black wins!" : "CPU wins";
        }
        banner(d, title, "Any button: play again");
      }
    });
  }
}

void runChess(TFT_eSPI& display, bool twoPlayer) {
  Session session(display, Button::Button3, BOARD_ROWS);
  uint32_t shown = 0;  // what is on the screen now
  // Only the computer needs the search's room, and it is too big for the stack.
  // Plain malloc: it just returns null if there is no room.
  std::unique_ptr<chess::Workspace, decltype(&free)> work(
      twoPlayer ? nullptr
                : static_cast<chess::Workspace*>(
                      malloc(sizeof(chess::Workspace))),
      &free);

  chess::State state;
  uint32_t seen[101];  // positions since the last capture or pawn move
  uint8_t seenCount = 0;
  uint8_t cursor = 0;
  int8_t selected = -1;
  int8_t movedFrom = -1, movedTo = -1;
  uint8_t result = chess::PLAYING;
  bool check = false;
  uint16_t wins = 0, losses = 0;
  uint32_t computerAt = 0;
  uint32_t since1 = 0;  // BTN1 went down: a tap moves, a hold picks or places
  Input in;
  chess::Move moves[chess::MAX_MOVES];
  uint16_t count = 0;

  const auto start = [&]() {
    chess::reset(state);
    seen[0] = chess::fingerprint(state);
    seenCount = 1;
    cursor = 6 * 8 + 4;  // the white king's pawn
    selected = movedFrom = movedTo = -1;
    result = chess::PLAYING;
    check = false;
    count = chess::legalMoves(state, moves);
  };

  const auto step = [&](const chess::Move move) {
    const bool taken = state.cells[move.to] != chess::EMPTY;
    chess::apply(state, move);
    movedFrom = move.from;
    movedTo = move.to;
    selected = -1;
    result = chess::result(state);
    check = chess::inCheck(state);
    count = chess::legalMoves(state, moves);
    if (state.quiet == 0) seenCount = 0;
    const uint32_t mark = chess::fingerprint(state);
    uint8_t before = 0;
    for (uint8_t i = 0; i < seenCount; ++i) {
      if (seen[i] == mark) ++before;
    }
    if (seenCount < 101) seen[seenCount++] = mark;
    if (result == chess::PLAYING && before >= 2) result = chess::DRAW;
    audioPlayTone(check ? 1568 : taken ? 1046 : 660, check ? 90 : 30);
    if (result == chess::WHITE_WINS) ++wins;
    if (result == chess::BLACK_WINS) ++losses;
    if (result != chess::PLAYING) {
      const bool cheer = result == chess::WHITE_WINS ||
                         (twoPlayer && result == chess::BLACK_WINS);
      audioPlayTone(cheer ? 1320 : 220, 200);
    }
    computerAt = millis() + 300;
  };

  start();
  while (session.next(in)) {
    const bool computer = !twoPlayer && state.turn == chess::BLACK;
    if (result != chess::PLAYING) {
      since1 = 0;
      if (in.pressed != Button::None) start();  // any button
    } else {
      const uint8_t row = cursor / 8;
      const uint8_t col = cursor % 8;
      if (in.pressed == Button::Button1) since1 = millis();
      if (in.pressed == Button::Button4) cursor = row * 8 + (col + 1) % 8;
      if (in.pressed == Button::Button2) cursor = (cursor + 56) % 64;
      if (in.pressed == Button::Button3) cursor = (cursor + 8) % 64;

      bool choose = false;
      if (since1 != 0) {
        if (in.held != Button::Button1) {  // a tap
          cursor = row * 8 + (col + 7) % 8;
          since1 = 0;
        } else if (millis() - since1 >= PLACE_HOLD_MS) {
          choose = true;
          since1 = 0;
        }
      }

      if (choose && !computer) {
        bool done = false;
        for (uint16_t i = 0; i < count && !done; ++i) {
          if (moves[i].from == selected && moves[i].to == cursor) {
            step(moves[i]);
            done = true;
          }
        }
        if (!done) {
          // Pick up a piece that can move; picking it again puts it down.
          bool movable = false;
          for (uint16_t i = 0; i < count; ++i) {
            if (moves[i].from == cursor) movable = true;
          }
          selected = movable && selected != cursor ? cursor : -1;
          audioPlayTone(movable ? 880 : 200, 30);
        }
      } else if (computer && millis() >= computerAt) {
        const auto pick = [](uint16_t n) {
          return static_cast<uint16_t>(random(n));
        };
        // Without room to think it can only pick any legal move.
        step(work ? chess::computerMove(state, CHESS_DEPTH, *work, seen,
                                        seenCount - 1, pick)
                  : moves[random(count)]);
      }
    }

    // Only draw when something has changed, so the screen holds still.
    const int32_t view[] = {cursor, selected, movedFrom, movedTo, result,
                            check, wins, losses,
                            static_cast<int32_t>(session.exitBar())};
    const uint32_t now =
        mix(chess::fingerprint(state), view, sizeof(view));
    if (now == shown) continue;
    shown = now;
    session.present([&](TFT_eSPI& d) {
      d.fillRect(0, 0, W, H, TFT_BLACK);
      for (uint8_t i = 0; i < 64; ++i) {
        const int16_t x = BOARD_LEFT + (i % 8) * SQUARE;
        const int16_t y = BOARD_TOP + (i / 8) * SQUARE;
        d.fillRect(x, y, SQUARE, SQUARE,
                   (i / 8 + i % 8) % 2 == 0 ? LIGHT_SQUARE : DARK_SQUARE);
        if (check && i == state.king[state.turn >> 3]) {
          d.fillRect(x, y, SQUARE, SQUARE, TFT_RED);
        }
        if (i == movedFrom || i == movedTo) {
          d.drawRect(x, y, SQUARE, SQUARE, TFT_CYAN);
        }
        if (i == selected) {
          d.drawRect(x, y, SQUARE, SQUARE, TFT_GREEN);
          d.drawRect(x + 1, y + 1, SQUARE - 2, SQUARE - 2, TFT_GREEN);
        }
        if (state.cells[i] != chess::EMPTY) {
          drawChessPiece(d, state.cells[i], x + 2, y + 2);
        }
      }
      // Where the picked-up piece may go.
      for (uint16_t i = 0; i < count; ++i) {
        if (moves[i].from != selected) continue;
        d.fillCircle(BOARD_LEFT + (moves[i].to % 8) * SQUARE + SQUARE / 2,
                     BOARD_TOP + (moves[i].to / 8) * SQUARE + SQUARE / 2, 4,
                     TFT_GREEN);
      }
      if (result == chess::PLAYING) {
        const int16_t x = BOARD_LEFT + (cursor % 8) * SQUARE;
        const int16_t y = BOARD_TOP + (cursor / 8) * SQUARE;
        d.drawRect(x, y, SQUARE, SQUARE, TFT_YELLOW);
        d.drawRect(x + 1, y + 1, SQUARE - 2, SQUARE - 2, TFT_YELLOW);
      }

      // Left of the board: whose turn and the controls. Right: the score.
      const bool whiteTurn = state.turn == chess::WHITE;
      text(d,
           twoPlayer ? (whiteTurn ? "WHT" : "BLK") : (whiteTurn ? "YOUR" : "CPU"),
           BOARD_LEFT / 2, 20, TFT_WHITE);
      text(d, "turn", BOARD_LEFT / 2, 32, TFT_WHITE);
      if (check) text(d, "CHECK", BOARD_LEFT / 2, 56, TFT_RED);
      const char* const help[] = {"1 left", "4 right", "2 up",
                                  "3 down", "hold 1",  "select"};
      for (uint8_t i = 0; i < 6; ++i) {
        text(d, help[i], BOARD_LEFT / 2, 140 + i * 13, TFT_LIGHTGREY);
      }
      const int16_t side = W - BOARD_LEFT / 2;
      char number[16];
      text(d, "wins", side, 96, TFT_LIGHTGREY);
      snprintf(number, sizeof(number), "%u-%u", wins, losses);
      text(d, number, side, 110, TFT_WHITE);

      if (result != chess::PLAYING) {
        const char* title = result == chess::STALEMATE ? "Stalemate" : "Draw";
        if (result == chess::WHITE_WINS) {
          title = twoPlayer ? "White wins!" : "You win!";
        }
        if (result == chess::BLACK_WINS) {
          title = twoPlayer ? "Black wins!" : "CPU wins";
        }
        banner(d, title, "Any button: play again");
      }
    });
  }
}

void runRockPaperScissors(TFT_eSPI& display, bool twoPlayer) {
  // Loaded first: the picture needs a large block of memory.
  bool night = true;
  FlappyAssets assets;
  loadFlappyAssets(assets, night);
  // BTN3 is a sign here. A whole-screen buffer does not fit beside the
  // picture, so it is drawn in strips of 48 rows.
  Session session(display, Button::Button4, 48);
  bool skipTap = false;  // this BTN4 press only closed the result
  // Second: two players, waiting for player 2's pick.
  enum class Phase : uint8_t { Choose, Second, Count, Reveal };
  Phase phase = Phase::Choose;
  uint8_t mine = 0, theirs = 0;
  uint16_t wins = 0, losses = 0, draws = 0;
  uint32_t countStart = 0;
  uint8_t lastStep = 0xFF;
  Input in;
  constexpr uint32_t STEP_MS = 350;

  while (session.next(in)) {
    int8_t choice = -1;
    if (phase == Phase::Reveal) {
      // The result stays up until a button closes it; that press picks nothing.
      if (in.pressed != Button::None) {
        phase = Phase::Choose;
        skipTap = in.pressed == Button::Button4;
      }
    } else if (phase != Phase::Count) {
      choice = in.pressed == Button::Button1   ? 0
               : in.pressed == Button::Button2 ? 1
               : in.pressed == Button::Button3 ? 2
                                               : -1;
    }
    // A tap of BTN4 switches between day and night.
    if (in.tap) {
      if (!skipTap) {
        night = !night;
        switchFlappyBackground(assets, night);
      }
      skipTap = false;
    }
    if (choice >= 0) {
      if (twoPlayer && phase != Phase::Second) {
        mine = choice;  // kept hidden until both have picked
        phase = Phase::Second;
        audioPlayTone(600, 40);
      } else {
        if (twoPlayer) {
          theirs = choice;
        } else {
          mine = choice;
          theirs = random(3);
        }
        phase = Phase::Count;
        countStart = millis();
        lastStep = 0xFF;
      }
    }
    uint8_t step = 0;
    if (phase == Phase::Count) {
      step = (millis() - countStart) / STEP_MS;
      if (step >= 3) {
        phase = Phase::Reveal;
        // Each sign beats the one before it: paper > rock, and so on.
        if (mine == theirs) {
          ++draws;
          audioPlayTone(500, 100);
        } else if (mine == (theirs + 1) % 3) {
          ++wins;
          audioPlayTone(1320, 150);
        } else {
          ++losses;
          audioPlayTone(220, 200);
        }
      } else if (step != lastStep) {
        lastStep = step;
        audioPlayTone(700, 40);
      }
    }

    const auto scene = [&](TFT_eSPI& d) {
      // Dark panels keep the words readable over the picture.
      d.fillRoundRect(70, 1, 160, 18, 4, TFT_BLACK);
      d.drawRoundRect(70, 1, 160, 18, 4, TFT_WHITE);
      if (phase != Phase::Reveal) {
        d.fillRoundRect(50, 44, 220, 60, 6, TFT_BLACK);
        d.drawRoundRect(50, 44, 220, 60, 6, TFT_WHITE);
      } else {
        d.fillRoundRect(30, 28, 260, 132, 6, TFT_BLACK);
        d.drawRoundRect(30, 28, 260, 132, 6, TFT_WHITE);
      }
      char score[40];
      snprintf(score, sizeof(score),
               twoPlayer ? "P1 %u   P2 %u   Draw %u"
                         : "You %u   CPU %u   Draw %u",
               wins, losses, draws);
      text(d, score, 150, 10, TFT_WHITE);
      if (phase == Phase::Choose) {
        text(d, twoPlayer ? "Player 1: pick" : "Pick your sign", W / 2, 60,
             TFT_YELLOW, 2);
        if (twoPlayer) text(d, "(player 2, look away)", W / 2, 90, TFT_WHITE);
      } else if (phase == Phase::Second) {
        text(d, "Player 2: pick", W / 2, 60, TFT_ORANGE, 2);
        text(d, "(player 1, look away)", W / 2, 90, TFT_WHITE);
      } else if (phase == Phase::Count) {
        text(d, HAND_NAMES[min<uint8_t>(step, 2)], W / 2, 90, TFT_YELLOW, 3);
      } else {
        text(d, twoPlayer ? "P1" : "You", 80, 40, TFT_WHITE, 2);
        text(d, twoPlayer ? "P2" : "CPU", 240, 40, TFT_WHITE, 2);
        drawHand(d, mine, 80, 90);
        drawHand(d, theirs, 240, 90);
        text(d, HAND_NAMES[mine], 80, 125, TFT_WHITE);
        text(d, HAND_NAMES[theirs], 240, 125, TFT_WHITE);
        const bool first = mine == (theirs + 1) % 3;
        text(d,
             mine == theirs ? "Draw"
             : twoPlayer    ? (first ? "P1 wins!" : "P2 wins!")
                            : (first ? "You win!" : "You lose"),
             W / 2, 148, TFT_YELLOW, 2);
      }
      for (uint8_t i = 0; i < 3; ++i) {
        const int16_t x = 56 + i * 104;
        d.fillRoundRect(x - 44, 164, 88, 70, 6, TFT_BLACK);
        d.drawRoundRect(x - 44, 164, 88, 70, 6,
                        (phase == Phase::Reveal ||
                         (!twoPlayer && phase == Phase::Count)) &&
                                mine == i
                            ? TFT_YELLOW
                            : TFT_WHITE);
        drawHand(d, i, x, 190);
        char label[20];
        snprintf(label, sizeof(label), "BTN%u %s", i + 1, HAND_NAMES[i]);
        text(d, label, x, 224, TFT_WHITE);
      }
      d.fillRoundRect(236, 1, 82, 18, 4, TFT_BLACK);
      d.drawRoundRect(236, 1, 82, 18, 4, TFT_WHITE);
      text(d, phase == Phase::Reveal ? "any: OK" : "4: day/night", 277, 10,
           TFT_LIGHTGREY);
    };
    session.showStrips(assets.background.data, 0x0010, scene);
  }
  assets.release();
}

void runBrickBreaker(TFT_eSPI& display) {
  Session session(display);
  SuperFlashyAssets eggArt;
  if (loadGameAssetImage("super/gold_egg.rgb565", "rgb565", eggArt.goldEgg)) {
    bindSuperFlashyArt(eggArt);
  }
  bool bricks[BRICK_ROWS][BRICK_COLS];
  uint16_t left = 0;
  uint8_t level = 1, lives = 3;
  uint32_t score = 0;
  float paddle = (W - PADDLE_W) / 2.0f;
  float x = 0, y = 0, vx = 0, vy = 0;
  float angle = 0, spin = 0;  // the egg tumbles
  bool launched = false;
  bool over = false;
  Input in;
  const uint16_t rowColors[BRICK_ROWS] = {TFT_RED, TFT_ORANGE, TFT_YELLOW,
                                          TFT_GREEN, TFT_CYAN};
  const auto fillBricks = [&]() {
    for (auto& row : bricks) {
      for (auto& b : row) b = true;
    }
    left = BRICK_ROWS * BRICK_COLS;
  };
  fillBricks();

  while (session.next(in)) {
    if (over) {
      if (in.tap) {
        over = false;
        level = 1;
        lives = 3;
        score = 0;
        launched = false;
        fillBricks();
      }
    } else {
      if (in.held == Button::Button1) paddle -= PADDLE_SPEED;
      if (in.held == Button::Button4) paddle += PADDLE_SPEED;
      paddle = constrain(paddle, 0.0f, static_cast<float>(W - PADDLE_W));
      const float speed = min(9.0f, 6.0f + 0.5f * (level - 1));

      if (!launched) {
        angle = 0;
        spin = 0;
        x = paddle + PADDLE_W / 2.0f;
        y = PADDLE_Y - EGG_B - 1;
        if (in.pressed == Button::Button3) {
          launched = true;
          vx = (random(2) == 0 ? -1 : 1) * speed * 0.5f;
          vy = -sqrtf(speed * speed - vx * vx);
        }
      } else {
        const float prevX = x, prevY = y;
        const float prevReach = eggReachY(angle);
        x += vx;
        y += vy;
        angle += spin;
        const float rx = eggReachX(angle);
        const float ry = eggReachY(angle);
        if (x < rx || x > W - rx) {
          vx = x < rx ? fabsf(vx) : -fabsf(vx);
          x = constrain(x, rx, W - rx);
          eggKick(vx, vy, spin, angle, speed, true, 1.0f);
        }
        if (y < 16 + ry) {
          vy = fabsf(vy);
          y = 16 + ry;
          eggKick(vx, vy, spin, angle, speed, false, 1.0f);
        }
        // Paddle: where it hits decides the angle.
        if (vy > 0 && prevY + prevReach <= PADDLE_Y + 1 && y + ry >= PADDLE_Y &&
            x >= paddle - rx && x <= paddle + PADDLE_W + rx) {
          const float offset = constrain(
              (x - paddle - PADDLE_W / 2.0f) / (PADDLE_W / 2.0f), -1.0f, 1.0f);
          vx = speed * offset * 0.85f;
          vy = -sqrtf(speed * speed - vx * vx);
          y = PADDLE_Y - ry;
          // Gentler off the paddle, so aiming still works.
          eggKick(vx, vy, spin, angle, speed, false, 0.6f);
          audioPlayTone(520, 20);
        }
        bool hit = false;
        for (uint8_t r = 0; r < BRICK_ROWS && !hit; ++r) {
          for (uint8_t c = 0; c < BRICK_COLS && !hit; ++c) {
            if (!bricks[r][c]) continue;
            const float bx = BRICK_LEFT + c * (BRICK_W + BRICK_GAP);
            const float by = BRICK_TOP + r * (BRICK_H + BRICK_GAP);
            if (x + rx < bx || x - rx > bx + BRICK_W || y + ry < by ||
                y - ry > by + BRICK_H) {
              continue;
            }
            hit = true;
            bricks[r][c] = false;
            --left;
            score += 10 * (BRICK_ROWS - r);
            // Came from the side if it was beside the brick a moment ago.
            const bool side =
                prevX + rx < bx || prevX - rx > bx + BRICK_W;
            if (side) {
              vx = -vx;
            } else {
              vy = -vy;
            }
            eggKick(vx, vy, spin, angle, speed, side, 1.0f);
            audioPlayTone(880, 20);
          }
        }
        if (left == 0) {
          ++level;
          launched = false;
          fillBricks();
          audioPlayTone(1320, 200);
        } else if (y > H + EGG_B) {
          launched = false;
          audioPlayTone(180, 200);
          if (--lives == 0) over = true;
        }
      }
    }

    TFT_eSPI& d = session.canvas();
    d.fillRect(0, 0, W, H, TFT_BLACK);
    char hud[48];
    snprintf(hud, sizeof(hud), "Score %lu    Level %u    Lives %u",
             static_cast<unsigned long>(score), level, lives);
    text(d, hud, W / 2, 8, TFT_WHITE);
    for (uint8_t r = 0; r < BRICK_ROWS; ++r) {
      for (uint8_t c = 0; c < BRICK_COLS; ++c) {
        if (!bricks[r][c]) continue;
        d.fillRect(BRICK_LEFT + c * (BRICK_W + BRICK_GAP),
                   BRICK_TOP + r * (BRICK_H + BRICK_GAP), BRICK_W, BRICK_H,
                   rowColors[r]);
      }
    }
    d.fillRoundRect(static_cast<int16_t>(paddle), PADDLE_Y, PADDLE_W, 6, 3,
                    TFT_WHITE);
    drawSpunEgg(d, x, y, angle);
    if (over) {
      banner(d, "Game over", "BTN3: play again");
    } else if (!launched) {
      text(d, "BTN3: launch    BTN1/4: move", W / 2, 160, TFT_LIGHTGREY);
    }
    session.show();
  }
  eggArt.release();
}

void runRunner(TFT_eSPI& display) {
  // The frame buffer has to come first. Loading the pictures first breaks the
  // heap into pieces, the full screen no longer fits, and the run is then
  // painted straight to the panel.
  Session session(display);
  FlappyAssets assets;
  SuperFlashyAssets sprites;
  const auto loadArt = [&]() {
    return loadFlappyAssets(assets, false, false) &&
           loadSuperFlashyAssets(sprites);
  };
  if (!loadArt()) {
    session.releaseBuffer();
    assets.release();
    sprites.release();
    if (!loadArt()) {
      display.fillScreen(TFT_BLACK);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(TFT_WHITE, TFT_BLACK);
      display.drawString("Game art unavailable", display.width() / 2,
                         display.height() / 2);
      delay(1200);
      return;
    }
    session.reserveBuffer();
  }
  if (!session.fullBuffer()) {
    sprites.sky.release();
    session.reserveBuffer();
  }
  bindSuperFlashyArt(sprites);
  uint32_t best = loadRunnerBest();
  float x = RUNNER_X;
  Obstacle obstacles[4] = {};
  float y = GROUND_Y, vy = 0;
  float speed = RUNNER_START_SPEED;
  float distance = 0;
  float nextGap = 0;
  float stride = 0;
  bool over = false;
  Input in;
  const uint16_t* const walk[] = {sfbWalk1, sfbWalk0, sfbWalk4, sfbWalk5,
                                  sfbWalk3, sfbWalk2, sfbWalk6, sfbWalk7};
  // The swooping Flappy Falcon: wing frames in order.
  const uint8_t flap[] = {0, 1, 2, 1};
  constexpr int16_t FALCON_ART = 48;
  constexpr int16_t FALCON = 30;  // drawn size
  constexpr int16_t FLY_HEIGHT = 22;  // above the ground: duck under it
  while (session.next(in)) {
    const bool ducking = in.held == Button::Button2 && y >= GROUND_Y;
    if (over) {
      if (in.pressed == Button::Button3) {
        for (auto& o : obstacles) o.active = false;
        y = GROUND_Y;
        vy = 0;
        speed = RUNNER_START_SPEED;
        distance = 0;
        nextGap = 0;
        x = RUNNER_X;
        over = false;
      }
    } else {
      if (in.pressed == Button::Button3 && y >= GROUND_Y) {
        vy = RUNNER_JUMP;
        audioPlayTone(660, 20);
      }
      if (in.held == Button::Button1) x -= RUNNER_STEP;
      if (in.held == Button::Button4) x += RUNNER_STEP;
      x = constrain(x, RUNNER_X - RUNNER_REACH, RUNNER_X + RUNNER_REACH);
      // Holding duck in the air drops faster.
      vy += in.held == Button::Button2 ? RUNNER_GRAVITY * 2.5f : RUNNER_GRAVITY;
      y += vy;
      if (y >= GROUND_Y) {
        y = GROUND_Y;
        vy = 0;
      }
      speed = min(RUNNER_TOP_SPEED, speed + RUNNER_SPEED_UP);
      distance += speed;
      stride += speed / 12.0f;

      nextGap -= speed;
      if (nextGap <= 0) {
        for (auto& o : obstacles) {
          if (o.active) continue;
          const bool flying = distance > 1500 && random(4) == 0;
          // A swooping falcon, or one or two gorillas (sometimes red ones).
          const bool pair = !flying && distance > 800 && random(3) == 0;
          o = {true, flying, static_cast<float>(W),
               static_cast<int16_t>(flying ? 28
                                           : sfbGorillaWidth * (pair ? 2 : 1)),
               static_cast<int16_t>(flying ? 20 : sfbGorillaHeight),
               random(3) == 0};
          nextGap = speed * 30 + 60 + random(100);
          break;
        }
      }
      // The bird's solid part is narrower than its 32 px picture.
      const float top = y - (ducking ? 14 : 26);
      for (auto& o : obstacles) {
        if (!o.active) continue;
        o.x -= speed;
        if (o.x + o.w < 0) o.active = false;
        const float bottom = o.flying ? GROUND_Y - FLY_HEIGHT : GROUND_Y;
        // A few pixels of grace around the enemies' pictures.
        const float at = o.x + sway(o);
        if (at + 3 < x + 24 && at + o.w - 3 > x + 8 &&
            top < bottom - 1 && y > bottom - o.h + 3) {
          over = true;
          if (distance / 10 > best) {  // only write when it is beaten
            best = distance / 10;
            saveRunnerBest(best);
          }
          audioPlayTone(160, 300);
        }
      }
    }

    session.present([&](TFT_eSPI& d) {
      if (session.fullBuffer() && sfbSkyRuns != nullptr) {
        copySuperFlashySky(session.buffer(),
                           static_cast<int32_t>(distance / 4) % W);
      } else {
        d.fillRect(0, 0, W, H, 0x5DFF);
      }
      const int16_t scroll = static_cast<int32_t>(distance) % 16;
      for (int16_t gx = -scroll; gx < W; gx += 16) {
        drawArt(d, gx, GROUND_Y, 16, 16, sfbGrass);
        for (int16_t gy = GROUND_Y + 16; gy < H; gy += 16) {
          drawArt(d, gx, gy, 16, 16, sfbDirt);
        }
      }
      for (const auto& o : obstacles) {
        if (!o.active) continue;
        const int16_t ox = static_cast<int16_t>(o.x + sway(o));
        if (o.flying) {  // facing the bird it is flying at
          const uint16_t* const frames[] = {assets.birdOpen.pixels(),
                                            assets.birdTransition.pixels(),
                                            assets.birdClosed.pixels()};
          const uint16_t* wing =
              frames[flap[(static_cast<int32_t>(distance) / 24) % 4]];
          if (wing != nullptr) {
            drawArt(d, ox - 1, GROUND_Y - FLY_HEIGHT - FALCON + 5, FALCON_ART,
                    FALCON_ART, wing, true, FALCON, FALCON);
          }
        } else {
          for (int16_t gx = 0; gx < o.w; gx += sfbGorillaWidth) {
            drawArt(d, ox + gx, GROUND_Y - o.h, sfbGorillaWidth,
                    sfbGorillaHeight, o.red ? sfbRedGorilla : sfbGorilla);
          }
        }
      }
      if (ducking) {
        drawArt(d, static_cast<int16_t>(x) + 16 - sfbDuckWidth / 2,
                static_cast<int16_t>(y) - sfbDuckHeight, sfbDuckWidth,
                sfbDuckHeight, sfbDuck);
      } else {
        const uint16_t* art =
            y < GROUND_Y ? sfbWalk1 : walk[static_cast<int>(stride) % 8];
        drawArt(d, static_cast<int16_t>(x), static_cast<int16_t>(y) - 32, 32,
                32, art);
      }
      char hud[32];
      snprintf(hud, sizeof(hud), "Score %lu   Best %lu",
               static_cast<unsigned long>(distance / 10),
               static_cast<unsigned long>(best));
      text(d, hud, W - 6, 10, TFT_BLACK, 1, MR_DATUM);
      text(d, "3 jump   hold 2 duck   1/4 nudge", W / 2, H - 10, TFT_WHITE);
      if (over) banner(d, "Game over", "BTN3: run again");
    });
  }
  // Quitting in the middle of a run still counts it.
  if (distance / 10 > best) saveRunnerBest(distance / 10);
  assets.release();
  sprites.release();
}

void runFlashyGolf(TFT_eSPI& display) {
  // The frame buffer comes before the pictures, as in Runner.
  Session session(display);
  SuperFlashyAssets sprites;
  if (!loadSuperFlashyAssets(sprites)) {
    session.releaseBuffer();
    sprites.release();
    if (!loadSuperFlashyAssets(sprites)) {
      display.fillScreen(TFT_BLACK);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(TFT_WHITE, TFT_BLACK);
      display.drawString("Game art unavailable", display.width() / 2,
                         display.height() / 2);
      delay(1200);
      return;
    }
    session.reserveBuffer();
  }
  if (!session.fullBuffer()) {
    sprites.sky.release();
    session.reserveBuffer();
  }
  bindSuperFlashyArt(sprites);

  uint16_t best[GOLF_COUNT];
  for (uint8_t i = 0; i < GOLF_COUNT; ++i) best[i] = loadGolfBest(i);
  bool selecting = true;
  uint8_t chosen = 0;
  uint32_t shown = 0;  // what the level screen last showed

  GolfBall ball = {};
  GolfHole hole = {};
  float restX = 0, restY = 0, camera = 0;
  float lift = 0;  // how far the view is raised: 0 or below
  float tilt = 0;  // radians, clockwise
  uint16_t flaps = 0;
  uint8_t wing = 0;  // frames left of the wing's downstroke
  bool sunk = false, newBest = false;
  uint32_t sunkAt = 0;
  Input in;

  const auto start = [&]() {
    const GolfLevel& level = GOLF_LEVELS[chosen];
    bool found = false;
    for (int16_t row = 0; row < GOLF_ROWS; ++row) {
      for (int16_t col = 0; col < level.columns; ++col) {
        if (level.rows[row][col] == 'S') {
          ball = {col * 16 + 8.0f, (row + 1) * 16 - GOLF_R, 0, 0, 0, 1};
        } else if (level.rows[row][col] == 'F' && !found) {
          hole = {static_cast<int16_t>(col * 16),
                  static_cast<int16_t>(row * 16)};
          found = true;
        }
      }
    }
    restX = ball.x;
    restY = ball.y;
    camera = constrain(ball.x - W / 2, 0.0f, level.columns * 16.0f - W);
    lift = 0;
    flaps = 0;
    wing = 0;
    tilt = 0;
    sunk = false;
    selecting = false;
  };

  while (session.next(in)) {
    if (selecting) {
      if (in.pressed == Button::Button1) {
        chosen = (chosen + GOLF_COUNT - 1) % GOLF_COUNT;
      }
      if (in.pressed == Button::Button4) chosen = (chosen + 1) % GOLF_COUNT;
      if (in.tap) {
        start();
        continue;
      }
      const uint32_t now = 1 + chosen + (session.exitBar() << 8);
      if (now == shown) continue;
      shown = now;
      session.present(
          [&](TFT_eSPI& d) { drawGolfSelect(d, chosen, best); });
      continue;
    }

    const GolfLevel& level = GOLF_LEVELS[chosen];
    if (in.tap) {  // back to the levels
      selecting = true;
      shown = 0;
      continue;
    }
    if (sunk) {
      // A moment first, so a late flap does not skip the result.
      if (millis() - sunkAt > 700) {
        if (in.pressed == Button::Button1 || in.pressed == Button::Button2) {
          start();
        } else if (in.pressed == Button::Button4) {
          if (chosen + 1 < GOLF_COUNT) {
            ++chosen;
            start();
          } else {
            selecting = true;
            shown = 0;
          }
          continue;
        }
      }
    } else if (in.pressed == Button::Button2) {
      start();
    } else if (in.pressed == Button::Button1 ||
               in.pressed == Button::Button4) {
      const int8_t side = in.pressed == Button::Button4 ? 1 : -1;
      golfFlap(ball, side);
      tilt += side * 0.35f;  // the flap throws it over that way
      if (flaps < 999) ++flaps;
      wing = GOLF_THRUST_FRAMES;
      audioPlayTone(740, 20);
    }

    const bool grounded = golfFrame(ball, level, hole);
    if (wing > 0) --wing;
    const bool resting =
        grounded && fabsf(ball.vx) < 0.15f && fabsf(ball.vy) < 0.3f;
    if (resting) ball.vx = 0;
    // It leans the way it flies, rolls along the ground and settles upright.
    if (tilt > PI) tilt -= 2 * PI;
    if (tilt < -PI) tilt += 2 * PI;
    if (!grounded) {
      tilt += (constrain(ball.vx * 0.16f, -0.7f, 0.7f) - tilt) * 0.15f;
    } else if (fabsf(ball.vx) > 0.6f) {
      tilt += ball.vx * 0.12f;
    } else {
      tilt *= 0.75f;
    }
    if (!sunk) {
      if (ball.x > hole.x + 4 && ball.x < hole.x + 28 && ball.y > hole.y + 5) {
        sunk = true;
        sunkAt = millis();
        newBest = best[chosen] == 0 || flaps < best[chosen];
        if (newBest) {
          best[chosen] = flaps;
          saveGolfBest(chosen, flaps);
        }
        audioPlayTone(flaps <= level.silver ? 1320 : 880, 200);
      } else if (ball.y > H + 20) {  // down a pit
        ball = {restX, restY, 0, 0, 0, 1};
        tilt = 0;
        audioPlayTone(200, 200);
      } else if (resting) {
        restX = ball.x;
        restY = ball.y;
      }
    }
    const float target =
        constrain(ball.x - W / 2, 0.0f, level.columns * 16.0f - W);
    camera += (target - camera) * 0.2f;
    lift += (min(0.0f, ball.y - 80) - lift) * 0.3f;

    session.present([&](TFT_eSPI& d) {
      const int16_t left = lroundf(camera);
      const int16_t up = lroundf(lift);
      if (session.fullBuffer() && sfbSkyRuns != nullptr) {
        copySuperFlashySky(session.buffer(), (left / 4) % W);
      } else {
        d.fillRect(0, 0, W, H, 0x5DFF);
      }
      for (int16_t col = left / 16;
           col < level.columns && col * 16 < left + W; ++col) {
        for (int16_t row = 0; row < GOLF_ROWS; ++row) {
          const char tile = level.rows[row][col];
          const uint16_t* art = tile == 'X' || tile == 'F' ? sfbGrass
                                : tile == 'H'              ? sfbDirt
                                : tile == 'B'              ? sfbBox
                                : tile == 'C'              ? sfbCloud
                                                           : nullptr;
          if (art != nullptr) {
            drawArt(d, col * 16 - left, row * 16 - up, 16, 16, art);
          }
        }
      }
      // The cup, then its flag post standing in it.
      const int16_t cup = hole.x - left;
      const int16_t top = hole.y - up;
      d.fillRect(cup + 4, top, 24, 14, TFT_BLACK);
      d.fillRect(cup + 15, top - 44, 2, 58, TFT_WHITE);
      d.fillTriangle(cup + 17, top - 44, cup + 17, top - 28, cup + 35,
                     top - 36, TFT_RED);

      // The golf egg, its wings down for a moment after each flap. The middle
      // of its picture sits a pixel high so the egg rests on the ground.
      drawGolfEgg(d, lroundf(ball.x) - left, lroundf(ball.y) - 1 - up, tilt,
                  wing > 0 ? golfEggDown : golfEggUp);

      char line[48];
      snprintf(line, sizeof(line), "Level %u   Flaps %u", chosen + 1, flaps);
      text(d, line, 6, 8, TFT_BLACK, 1, ML_DATUM);
      golfStar(d, W - 76, 9, 7, GOLF_GOLD, flaps <= level.gold);
      snprintf(line, sizeof(line), "%u", level.gold);
      text(d, line, W - 64, 9, TFT_BLACK, 1, ML_DATUM);
      golfStar(d, W - 34, 9, 7, GOLF_SILVER, flaps <= level.silver);
      snprintf(line, sizeof(line), "%u", level.silver);
      text(d, line, W - 22, 9, TFT_BLACK, 1, ML_DATUM);
      text(d, "1/4 flap  2 restart  3 levels", W / 2, 22, TFT_BLACK);

      if (sunk) {
        d.fillRoundRect(40, 60, W - 80, 110, 8, TFT_BLACK);
        d.drawRoundRect(40, 60, W - 80, 110, 8, TFT_YELLOW);
        snprintf(line, sizeof(line), "In the hole: %u", flaps);
        text(d, line, W / 2, 76, TFT_YELLOW, 2);
        golfStar(d, W / 2 - 28, 112, 20, GOLF_GOLD, flaps <= level.gold);
        golfStar(d, W / 2 + 28, 112, 20, GOLF_SILVER, flaps <= level.silver);
        text(d, newBest ? "New best!" : "", W / 2, 140, TFT_WHITE);
        text(d,
             chosen + 1 < GOLF_COUNT ? "1 again  4 next level  3 levels"
                                     : "1 again  4 or 3: levels",
             W / 2, 156, TFT_WHITE);
      }
    });
  }
  sprites.release();
}

void runPong(TFT_eSPI& display, bool twoPlayer) {
  Session session(display);
  float player = (H - PONG_PADDLE_H) / 2.0f;
  float cpu = player;
  float x = W / 2.0f, y = H / 2.0f, vx = 0, vy = 0;
  float speed = 4.0f;
  float aim = 0;  // how far off center the computer tries to hit
  uint8_t mine = 0, theirs = 0;
  bool serving = true;
  int8_t serveTo = 1;
  // Two players: each paddle keeps moving; a tap turns it around. (Holding
  // buttons would not work, as only one can be read at a time.)
  int8_t playerDir = 0, cpuDir = 0;
  Input in;

  while (session.next(in)) {
    const bool over = mine >= PONG_WIN || theirs >= PONG_WIN;
    if (over) {
      if (in.tap) {
        mine = theirs = 0;
        serving = true;
      }
    } else {
      if (twoPlayer) {
        if (in.pressed == Button::Button1) playerDir = playerDir < 0 ? 1 : -1;
        if (in.pressed == Button::Button4) cpuDir = cpuDir < 0 ? 1 : -1;
        player += playerDir * PONG_TAP_SPEED;
        cpu = constrain(cpu + cpuDir * PONG_TAP_SPEED,
                        static_cast<float>(PONG_TOP),
                        static_cast<float>(H - PONG_PADDLE_H));
      } else {
        if (in.held == Button::Button1) player -= PONG_PLAYER_SPEED;
        if (in.held == Button::Button2) player += PONG_PLAYER_SPEED;
      }
      player = constrain(player, static_cast<float>(PONG_TOP),
                         static_cast<float>(H - PONG_PADDLE_H));
      if (serving) {
        x = W / 2.0f;
        y = (H + PONG_TOP) / 2.0f;
        if (in.pressed == Button::Button3) {
          serving = false;
          speed = 4.0f;
          vx = serveTo * speed * 0.9f;
          vy = random(-20, 21) / 10.0f;
          aim = random(-18, 19);
        }
      } else {
        // The computer chases the ball while it is coming, and is not perfect.
        if (!twoPlayer) {
          const float target =
              (vx > 0 ? y + aim : (H + PONG_TOP) / 2.0f) - PONG_PADDLE_H / 2.0f;
          cpu += constrain(target - cpu, -PONG_CPU_SPEED, PONG_CPU_SPEED);
          cpu = constrain(cpu, static_cast<float>(PONG_TOP),
                          static_cast<float>(H - PONG_PADDLE_H));
        }

        const float prevX = x;
        x += vx;
        y += vy;
        if (y < PONG_TOP + 3 || y > H - 3) {
          vy = -vy;
          y = constrain(y, PONG_TOP + 3.0f, H - 3.0f);
          audioPlayTone(330, 15);
        }
        // Bounce when the ball crosses a paddle's face during this tick.
        const auto bounce = [&](float paddleY, float dir) {
          speed = min(8.0f, speed + 0.25f);
          const float offset = constrain(
              (y - paddleY - PONG_PADDLE_H / 2.0f) / (PONG_PADDLE_H / 2.0f),
              -1.0f, 1.0f);
          vy = speed * offset * 0.75f;
          vx = dir * sqrtf(speed * speed - vy * vy);
          aim = random(-24, 25);
          audioPlayTone(660, 20);
        };
        const float leftFace = PONG_LEFT + PONG_PADDLE_W + 3;
        const float rightFace = PONG_RIGHT - 3;
        if (vx < 0 && prevX >= leftFace && x < leftFace && y >= player - 3 &&
            y <= player + PONG_PADDLE_H + 3) {
          x = leftFace;
          bounce(player, 1);
        } else if (vx > 0 && prevX <= rightFace && x > rightFace &&
                   y >= cpu - 3 && y <= cpu + PONG_PADDLE_H + 3) {
          x = rightFace;
          bounce(cpu, -1);
        }
        if (x < -6 || x > W + 6) {
          if (x < 0) {
            ++theirs;
            serveTo = -1;
          } else {
            ++mine;
            serveTo = 1;
          }
          serving = true;
          playerDir = cpuDir = 0;
          audioPlayTone(x < 0 ? 200 : 1046, 150);
        }
      }
    }

    TFT_eSPI& d = session.canvas();
    d.fillRect(0, 0, W, H, TFT_BLACK);
    for (int16_t ly = PONG_TOP; ly < H; ly += 14) {
      d.fillRect(W / 2 - 1, ly, 2, 8, TFT_DARKGREY);
    }
    char score[8];
    snprintf(score, sizeof(score), "%u", mine);
    text(d, score, W / 2 - 30, 12, TFT_WHITE, 2);
    snprintf(score, sizeof(score), "%u", theirs);
    text(d, score, W / 2 + 30, 12, TFT_WHITE, 2);
    d.fillRect(PONG_LEFT, static_cast<int16_t>(player), PONG_PADDLE_W,
               PONG_PADDLE_H, TFT_CYAN);
    d.fillRect(PONG_RIGHT, static_cast<int16_t>(cpu), PONG_PADDLE_W,
               PONG_PADDLE_H, TFT_ORANGE);
    d.fillRect(static_cast<int16_t>(x) - 3, static_cast<int16_t>(y) - 3, 6, 6,
               TFT_WHITE);
    if (mine >= PONG_WIN || theirs >= PONG_WIN) {
      const char* title = mine > theirs
                              ? (twoPlayer ? "Left wins!" : "You win!")
                              : (twoPlayer ? "Right wins!" : "CPU wins");
      banner(d, title, "BTN3: play again");
    } else if (serving) {
      text(d,
           twoPlayer ? "3 serve   tap 1: left   tap 4: right"
                     : "BTN3: serve    1 up  2 down",
           W / 2, H - 12, TFT_LIGHTGREY);
    }
    session.show();
  }
}

void drawTicTacToeIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  const int16_t left = x + 27, top = y + 3;
  for (int8_t i = 1; i < 3; ++i) {
    d.fillRect(left + i * 14 - 1, top, 2, 42, TFT_DARKGREY);
    d.fillRect(left, top + i * 14 - 1, 42, 2, TFT_DARKGREY);
  }
  const auto cross = [&](int16_t cx, int16_t cy) {
    d.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, TFT_CYAN);
    d.drawLine(cx + 4, cy - 4, cx - 4, cy + 4, TFT_CYAN);
  };
  cross(left + 7, top + 7);
  cross(left + 21, top + 21);
  cross(left + 35, top + 35);
  d.drawCircle(left + 35, top + 7, 5, TFT_ORANGE);
  d.drawCircle(left + 7, top + 35, 5, TFT_ORANGE);
}

void drawCheckersIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  const int16_t left = x + 24, top = y + 1;
  for (uint8_t i = 0; i < 36; ++i) {
    const uint8_t row = i / 6, col = i % 6;
    const int16_t sx = left + col * 8, sy = top + row * 8;
    const bool dark = (row + col) % 2 != 0;
    d.fillRect(sx, sy, 8, 8, dark ? DARK_SQUARE : LIGHT_SQUARE);
    if (!dark || row == 2 || row == 3) continue;
    d.fillCircle(sx + 4, sy + 4, 3, row < 2 ? TFT_BLACK : TFT_RED);
  }
}

void drawChessIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  const int16_t left = x + 20, top = y + 6;
  for (uint8_t i = 0; i < 2; ++i) {
    d.fillRect(left + i * 28, top, 28, 36, i == 0 ? DARK_SQUARE : LIGHT_SQUARE);
  }
  drawChessPiece(d, chess::KING, left + 2, top + 6);
  drawChessPiece(d, chess::KNIGHT | chess::BLACK, left + 30, top + 6);
}

void drawFlashyGolfIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  const int16_t left = x + 16;
  for (int16_t i = 0; i < 4; ++i) {
    drawArt(d, left + i * 16, y + 30, 16, 16, sfbGrass);
  }
  d.fillRect(left + 40, y + 30, 18, 8, TFT_BLACK);
  d.fillRect(left + 48, y + 2, 2, 36, TFT_WHITE);
  d.fillTriangle(left + 50, y + 2, left + 50, y + 14, left + 63, y + 8,
                 TFT_RED);
  drawArt(d, left + 10, y + 14, 20, 16, golfEggUp);
}

void drawRockPaperScissorsIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  d.fillCircle(x + 20, y + 24, 12, TFT_DARKGREY);
  d.drawCircle(x + 20, y + 24, 12, TFT_WHITE);
  d.fillRect(x + 38, y + 10, 20, 28, TFT_WHITE);
  for (int16_t line = 16; line <= 32; line += 6) {
    d.drawFastHLine(x + 41, y + line, 14, TFT_BLUE);
  }
  d.drawLine(x + 66, y + 10, x + 80, y + 30, TFT_LIGHTGREY);
  d.drawLine(x + 80, y + 10, x + 66, y + 30, TFT_LIGHTGREY);
  d.drawCircle(x + 66, y + 35, 4, TFT_RED);
  d.drawCircle(x + 80, y + 35, 4, TFT_RED);
}

void drawPongIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  d.fillRect(x + 18, y + 8, 5, 22, TFT_CYAN);
  d.fillRect(x + 73, y + 18, 5, 22, TFT_ORANGE);
  for (int16_t ly = 4; ly < 44; ly += 9) {
    d.fillRect(x + 47, y + ly, 2, 5, TFT_DARKGREY);
  }
  d.fillRect(x + 56, y + 17, 6, 6, TFT_WHITE);
}

void drawRunnerThumbnail(TFT_eSPI& d, const uint16_t* falcon, int16_t x,
                         int16_t y) {
  drawSuperFlashySkyThumbnail(d, x, y, 80, 58);
  for (int16_t i = 0; i < 5; ++i) {
    drawArt(d, x + i * 16, y + 42, 16, 16, sfbGrass);
  }
  if (falcon != nullptr) {
    drawArt(d, x + 42, y + 3, 48, 48, falcon, true, 24, 24);
  }
  drawArt(d, x + 54, y + 22, sfbGorillaWidth, sfbGorillaHeight, sfbRedGorilla);
  drawArt(d, x + 3, y + 11, 32, 32, sfbWalk0);
}

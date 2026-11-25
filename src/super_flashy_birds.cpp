#include "super_flashy_birds.h"

#include <Arduino.h>

#include <cmath>

#include "audio_helper.h"
#include "button_input.h"
#include "super_flashy_levels.h"
#include "super_flashy_sprites.h"

// Controls (one resistor-ladder ADC, so only one button reads at a time):
//   BTN1 left, BTN4 right, BTN3 jump, BTN2 duck (fast-fall in the air).
//   Double-press BTN1/BTN4 and keep holding it to run.
//   Hold BTN3 for 3 seconds to exit back to the menu.
// Because buttons cannot be held together, horizontal speed carries through a
// jump: releasing a direction keeps the speed for MOMENTUM_GRACE_MS (long enough
// to reach the jump button), and BTN1/BTN4 steer in the air.
namespace {
constexpr int16_t SCALE = 1;  // art pixels -> screen pixels (tiles match the 16 px boxes)
constexpr int16_t TILE = 16 * SCALE;
// The bird is drawn at its native art resolution (1:1) so it stays crisp and
// is one tile tall; the ground tiles use SCALE.
constexpr int16_t BIRD_SCALE = 1;
// Walk/idle frames are 32x32 art; the body (hitbox) is the 16 px in the middle.
constexpr int16_t PLAYER_W = 16 * BIRD_SCALE;
constexpr int16_t PLAYER_H = 32 * BIRD_SCALE;
constexpr int16_t DUCK_H = 16 * BIRD_SCALE;  // crouched: exactly one block tall
constexpr int16_t FRAME_ART = 32 * BIRD_SCALE;
// Walk animation advances at a fixed rate per game tick (30 ms): a whole
// number of ticks per frame keeps every frame on screen equally long.
constexpr float WALK_FRAMES_PER_TICK = 1.0f / 3.0f;  // 90 ms per frame
constexpr float RUN_FRAMES_PER_TICK = 1.0f / 2.0f;   // 60 ms per frame
constexpr int WALK_FRAME_COUNT = 8;
constexpr int16_t BIRD_SINK = 0;  // flat 2D: feet stand right on the surface
constexpr int16_t MAX_ROWS = LEVEL_MAX_ROWS;  // tallest level, in tiles

// ---- Levels -----------------------------------------------------------------
// Levels are drawn as text maps in levels/*.txt (see levels/README.md);
// tools/build_levels.py turns them into include/super_flashy_levels.h, which
// defines `Level` and the LEVELS table. PlatformIO runs it before each build.
static_assert(TILE == 16, "levels/*.txt maps assume 16 px tiles");
constexpr uint8_t MAX_GORILLAS = LEVEL_MAX_GORILLAS;
constexpr uint8_t BOX_MAX = LEVEL_MAX_BOXES;
// The slots after the boxes hold the nests.
constexpr uint8_t BOX_SLOTS = BOX_MAX + LEVEL_MAX_NESTS;
constexpr uint8_t FOX_MAX = LEVEL_MAX_FOXES;

const Level* level = &LEVELS[0];

int16_t worldW() { return level->tiles * TILE; }

constexpr uint16_t SKY = 0x5DFF;
constexpr uint16_t CLOUD = TFT_WHITE;

constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t DOUBLE_PRESS_MS = 500;
constexpr uint32_t MOMENTUM_GRACE_MS = 350;
constexpr uint32_t JUMP_BUFFER_MS = 120;
constexpr float CROUCH_SPEED = 1.2f;  // slow walk while crouched
constexpr float STOMP_BOUNCE = -8.0f;
constexpr float PUSH_SPEED = 3.5f;  // knockback speed when a gorilla bumps the bird
constexpr uint32_t EXIT_HOLD_MS = 3000;
constexpr uint32_t EXIT_SHOW_MS = 1500;

constexpr float GRAVITY = 0.7f;
constexpr float JUMP_VELOCITY = -11.1f;  // full jump peaks about 82 px up
constexpr float JUMP_CUT_VELOCITY = -4.0f;  // released jump button early
constexpr float MAX_FALL = 14.0f;
constexpr float GLIDE_GRAVITY_FACTOR = 0.5f / 3.0f;  // pressing jump again midair
constexpr float GLIDE_MAX_FALL_FACTOR = 0.5f / 3.0f;
constexpr float GLIDE_SPEED_FACTOR = 1.5f;  // horizontal speed while gliding
constexpr float WALK_SPEED = 2.5f;
constexpr float RUN_SPEED = 4.5f;
constexpr float ACCEL = 0.5f;
constexpr float GROUND_FRICTION = 0.45f;
constexpr float AIR_ACCEL = 0.25f;
// Walking off the end of the ground: the edge tile's slope is not solid.
constexpr int16_t EDGE_INSET = 8 * SCALE;
constexpr int16_t FALL_RESET_DEPTH = 120;

// Gorillas: red ones always walk toward the player; normal ones patrol back
// and forth a few blocks around their start. Both occasionally hop a little.
constexpr int16_t GORILLA_W = 23;
constexpr int16_t GORILLA_H = 20;
constexpr int16_t GORILLA_SINK = 0;
constexpr int16_t PATROL_RANGE = 6 * TILE;  // blocks each way from home
constexpr float PATROL_SPEED = 1.1f;
constexpr float CHASE_SPEED = 1.6f;
constexpr int GORILLA_HOP_ONE_IN = 200;  // per tick (~6 s on average)
constexpr float GORILLA_HOP_VELOCITY = -4.2f;  // small hop, about 12 px high

struct Player {
  float x;   // world x of the left side
  float y;   // world y of the feet
  float vx;
  float vy;
  bool onGround;
  bool facingRight;
  bool ducking;
  bool running;
  float stride;  // distance walked, drives the walk animation
};

struct Gorilla {
  float x;  // world x of the left side
  float y;  // world y of the feet
  float vy;
  float homeX;
  int8_t dir;  // patrol direction (normal gorillas)
  bool red;
  bool alive;
  float groundY;  // surface the gorilla walks on
  int16_t homeRow;  // the row it stands in; it stays on ground of this level
};

// Feet y of something standing in map row `row` (on the ground below it).
int16_t spawnY(int16_t row, int16_t screenH) { return screenH - row * TILE; }

int16_t columnAt(float worldX) {
  return static_cast<int16_t>(floorf(worldX / TILE));
}

bool solidTile(int16_t col, int16_t row) {
  if (col < 0 || col >= level->tiles || row < 0 || row >= 32) return false;
  return (level->solid[col] >> row) & 1u;
}

// ---- Direction blocks -------------------------------------------------------
// Solid tiles that the bird can pass through only while moving the way their
// arrow points, except that it can also jump up through any of them but a down
// one. To enemies they are ordinary tiles. The order matches the masks in
// Level::arrows.
enum ArrowDir : int8_t {
  ARROW_NONE = -1,
  ARROW_RIGHT,
  ARROW_LEFT,
  ARROW_UP,
  ARROW_DOWN
};

int8_t arrowAt(int16_t col, int16_t row) {
  if (col < 0 || col >= level->tiles || row < 0 || row >= 32) return ARROW_NONE;
  for (int8_t dir = ARROW_RIGHT; dir <= ARROW_DOWN; ++dir) {
    if ((level->arrows[col][dir] >> row) & 1u) return dir;
  }
  return ARROW_NONE;
}

// A solid tile that is not a direction block.
bool plainTile(int16_t col, int16_t row) {
  return solidTile(col, row) && arrowAt(col, row) == ARROW_NONE;
}

// True if the tile stops a bird that is moving in direction `moving`.
bool blocksBird(int16_t col, int16_t row, ArrowDir moving) {
  return solidTile(col, row) && arrowAt(col, row) != moving;
}

// True if the tile stops the head of a rising bird.
bool blocksJump(int16_t col, int16_t row) {
  return plainTile(col, row) ||
         (solidTile(col, row) && arrowAt(col, row) == ARROW_DOWN);
}

// ---- Boxes, eggs and rats ---------------------------------------------------
// Boxes are half a tile (16 px) and float freely, so they use rectangle
// collision instead of the tile grid. Plain boxes break when the bird bumps
// them from below. Mystery boxes stay, darken and push an egg out of the top.
// Eggs are the powerups (red: higher jump, blue: faster); now and then one is
// rotten: it hops and cracks open when the bird gets close, and a rat comes
// out that chases the bird and bites.
enum class BoxKind : uint8_t { Plain, Mystery, Nest };

constexpr int16_t BOX_SIZE = TILE;  // boxes are exactly one tile

struct Box {
  int16_t x;  // world x of the left side
  int16_t y;  // top edge (screen y)
  int16_t w;
  int16_t h;
  BoxKind kind;
  bool broken;
  bool used;  // mystery box already opened
  uint8_t bump;  // little hop animation after a hit
};

enum class EggState : uint8_t { None, Emerging, Idle, Hop, Open };

struct Egg {
  EggState state;
  float x;  // world x of the left side
  float y;  // world y of the bottom
  float vy;
  float restY;  // top of the box it sits on
  uint8_t timer;
  bool rotten;  // cracks open into a rat instead of being a powerup
  bool blue;    // blue = speed, red = jump
};

constexpr int16_t RAT_W = 24;
constexpr int16_t RAT_H = 12;
constexpr int16_t RAT_SINK = 0;

struct Rat {
  bool active;
  float x;  // world x of the left side
  float y;  // world y of the feet
  float vy;
  bool facingRight;
};

// Timed effects and health for the bird.
struct Status {
  int hp;
  uint32_t invulnUntil;
  uint32_t jumpBoostUntil;
  uint32_t speedBoostUntil;
};

constexpr int16_t EGG_SIZE = 16;
constexpr float EGG_EMERGE_SPEED = 2.0f;
constexpr float EGG_HOP_VELOCITY = -5.5f;
constexpr float EGG_TRIGGER_DIST = 44.0f;  // a rotten egg wakes at this distance
constexpr uint8_t EGG_OPEN_TICKS = 25;  // the broken shell stays this long
constexpr int ROTTEN_ONE_IN = 5;        // chance an egg is rotten
constexpr uint32_t BOOST_MS = 10000;
constexpr float BOOST_JUMP_FACTOR = 1.2f;
constexpr float BOOST_SPEED_FACTOR = 1.3f;
constexpr float RAT_SPEED = 1.5f;
constexpr int PLAYER_MAX_HP = 3;
constexpr uint32_t INVULN_MS = 1500;
constexpr float HURT_KNOCKBACK = 3.5f;
constexpr float HURT_POP = -5.0f;

constexpr int16_t NEST_W = sfbNestWidth;
constexpr int16_t NEST_H = sfbNestHeight;
constexpr int16_t NEST_SINK = 0;

Box boxes[BOX_SLOTS];  // slots from BOX_MAX on are nests (goals)
Egg eggs[BOX_SLOTS];   // eggs[i] belongs to boxes[i]
Rat rats[BOX_SLOTS];   // rats[i] hatched from eggs[i]

// Foxes: very quick. They sleep until the bird is near, race at it, crouch,
// leap high, and at the top of the leap pounce forward.
enum class FoxState : uint8_t { Idle, Prowl, Windup, Leap, Pounce, Recover };

constexpr int16_t FOX_W = sfbFoxWidth;   // sizes follow the art
constexpr int16_t FOX_H = sfbFoxHeight;
constexpr float FOX_RUN_SPEED = 2.8f;          // faster than a walking bird
constexpr float FOX_WAKE_DIST = 180.0f;
constexpr float FOX_POUNCE_RANGE = 110.0f;
constexpr uint8_t FOX_WINDUP_TICKS = 25;  // ~0.75 s crouch before the leap
constexpr int16_t FOX_SINK = 0;
constexpr uint8_t FOX_RECOVER_TICKS = 35;
constexpr float FOX_LEAP_VELOCITY = -10.5f;    // about 74 px, a bit lower than the bird
constexpr float FOX_POUNCE_VX = 6.0f;
constexpr float FOX_POUNCE_VY = 2.0f;

struct Fox {
  bool active;
  float x;  // world x of the left side
  float y;  // world y of the feet
  float vx;
  float vy;
  FoxState state;
  uint8_t timer;
  bool facingRight;
};

Fox foxes[FOX_MAX];

// Golden eggs: collectibles that hang where the map puts them. Dying restarts
// the level, so they all come back.
bool goldTaken[LEVEL_MAX_GOLDS];
uint8_t goldCount = 0;  // collected on this attempt

int16_t rowTop(int16_t row, int16_t screenH) {
  return screenH - (row + 1) * TILE;
}

int16_t rowBottom(int16_t row, int16_t screenH) { return screenH - row * TILE; }

// The bird's solid box: from its (visible) head down to the logical feet.
float boxHeight(const Player& p) {
  return (p.ducking ? DUCK_H : PLAYER_H) - BIRD_SINK;
}

// Block the player at the sides of solid tiles. Only a bird that moved
// sideways into a tile is pushed back out; one already beside or over it
// (walking off a ledge, falling past it) just keeps falling, which avoids
// snapping sideways at cliff edges.
void resolveWalls(Player& p, float prevX, int16_t screenH) {
  const float top = p.y - boxHeight(p);
  const float bottom = p.y;
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t c = first; c <= last; ++c) {
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      if (!solidTile(c, r)) continue;
      if (rowTop(r, screenH) >= bottom - 1.0f || rowBottom(r, screenH) <= top) {
        continue;
      }
      if (p.vy < 0 && !blocksJump(c, r) &&
          top - p.vy >= rowBottom(r, screenH) - 1.0f) {
        continue;  // jumping up into a direction block from below
      }
      if (prevX + PLAYER_W <= c * TILE + 0.01f) {
        if (!blocksBird(c, r, ARROW_RIGHT)) continue;
        p.x = c * TILE - PLAYER_W;  // came from the left
        p.vx = 0;
      } else if (prevX >= (c + 1) * TILE - 0.01f) {
        if (!blocksBird(c, r, ARROW_LEFT)) continue;
        p.x = (c + 1) * TILE;  // came from the right
        p.vx = 0;
      }
      break;
    }
  }
  // Floating boxes: push back out only if the bird came in from the side.
  for (const auto& b : boxes) {
    if (b.broken) continue;
    if (b.y >= bottom - 1.0f || b.y + b.h <= top) continue;
    if (p.x + PLAYER_W <= b.x || p.x >= b.x + b.w) continue;
    if (prevX + PLAYER_W <= b.x + 0.01f) {
      p.x = b.x - PLAYER_W;
      p.vx = 0;
    } else if (prevX >= b.x + b.w - 0.01f) {
      p.x = b.x + b.w;
      p.vx = 0;
    }
  }
}

// How far (px) the bird's box must overlap a column to stand on it. Landing on
// any overlapped column, not just the one under the center, means the bird
// can't fall through the corner of a block it jumped onto.
constexpr float SUPPORT_OVERLAP = 4.0f;

float columnOverlap(const Player& p, int16_t col) {
  const float left = max(p.x, static_cast<float>(col * TILE));
  const float right = min(p.x + PLAYER_W, static_cast<float>((col + 1) * TILE));
  return right - left;
}

// Top of the highest intact box under a span (at or below `fromY`), or 1e9.
float boxSurface(float x, float w, float fromY, float minOverlap) {
  float best = 1e9f;
  for (const auto& b : boxes) {
    if (b.broken) continue;
    const float overlap = min(x + w, static_cast<float>(b.x + b.w)) -
                          max(x, static_cast<float>(b.x));
    if (overlap < minOverlap) continue;
    if (b.y >= fromY - 4 && b.y < best) best = b.y;
  }
  return best;
}

// Land on the highest walkable surface the feet reach this tick.
bool landOnSurface(Player& p, int16_t screenH) {
  if (p.vy < 0) return false;
  const float prevFeet = p.y - p.vy;
  float best = 1e9f;
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t col = first; col <= last; ++col) {
    if (columnOverlap(p, col) < SUPPORT_OVERLAP) continue;
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      // A direction block above does not hide the floor under it.
      if (!blocksBird(col, r, ARROW_DOWN) || plainTile(col, r + 1)) continue;
      const float surface = rowTop(r, screenH);
      if (surface >= prevFeet - 4 && surface < best) best = surface;
    }
  }
  best = min(best, boxSurface(p.x, PLAYER_W, prevFeet, SUPPORT_OVERLAP));
  if (best < 1e8f && p.y >= best) {
    p.y = best;
    p.vy = 0;
    return true;
  }
  return false;
}

// Y of the highest surface at or below `fromY` under a span of width `w`
// (needs `minOverlap` px over a column), or a negative value if there is none.
// `forBird` applies the direction block rules.
float surfaceBelow(float x, float w, float fromY, float minOverlap,
                   int16_t screenH, bool forBird = false) {
  float best = 1e9f;
  const int16_t first = columnAt(x);
  const int16_t last = columnAt(x + w - 0.01f);
  for (int16_t col = first; col <= last; ++col) {
    const float overlap = min(x + w, static_cast<float>((col + 1) * TILE)) -
                          max(x, static_cast<float>(col * TILE));
    if (overlap < minOverlap) continue;
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      const bool top = forBird ? blocksBird(col, r, ARROW_DOWN) &&
                                     !plainTile(col, r + 1)
                               : solidTile(col, r) && !solidTile(col, r + 1);
      if (!top) continue;
      const float surface = rowTop(r, screenH);
      if (surface >= fromY - 4 && surface < best) best = surface;
    }
  }
  best = min(best, boxSurface(x, w, fromY, minOverlap));
  return best < 1e8f ? best : -1.0f;
}

// Where the bird would land (for its shadow); negative over a pit.
float landingY(const Player& p, int16_t screenH) {
  return surfaceBelow(p.x, PLAYER_W, p.y, SUPPORT_OVERLAP, screenH, true);
}

// Last line of defence: if the bird still ended up inside a tile (a deep
// overlap), move it out by the shortest way instead of letting it sit inside.
void unstick(Player& p, int16_t screenH) {
  const float top = p.y - boxHeight(p);
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t c = first; c <= last; ++c) {
    if (columnOverlap(p, c) < SUPPORT_OVERLAP) continue;
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      // Direction blocks are skipped: the bird may be passing through one.
      if (!plainTile(c, r)) continue;
      const float tileTop = rowTop(r, screenH);
      const float tileBottom = rowBottom(r, screenH);
      if (tileTop >= p.y - 2.0f || tileBottom <= top) continue;  // not inside
      const float up = p.y - tileTop;
      const float left = p.x + PLAYER_W - c * TILE;
      const float right = (c + 1) * TILE - p.x;
      if (!solidTile(c, r + 1) && up <= left && up <= right) {
        p.y = tileTop;  // stand on it
        p.vy = 0;
        p.onGround = true;
      } else if (left <= right) {
        p.x = c * TILE - PLAYER_W;
        p.vx = 0;
      } else {
        p.x = (c + 1) * TILE;
        p.vx = 0;
      }
      return;
    }
  }
}

// The bird bumped a box from below.
void hitBox(Box& b) {
  if (b.kind == BoxKind::Nest) return;
  if (b.kind == BoxKind::Plain) {
    b.broken = true;
    audioPlayTone(330, 40);
    return;
  }
  if (b.used) return;
  b.used = true;
  b.bump = 6;
  audioPlayTone(880, 40);
  Egg& e = eggs[&b - boxes];
  e.restY = static_cast<float>(b.y);
  e.x = b.x + (b.w - EGG_SIZE) / 2.0f;
  e.y = e.restY + EGG_SIZE;  // starts hidden inside the box
  e.vy = 0;
  e.timer = 0;
  e.rotten = random(ROTTEN_ONE_IN) == 0;
  e.blue = random(2) == 0;
  e.state = EggState::Emerging;
}

// Bump the head on the underside of a tile or box while rising.
void bumpHead(Player& p, int16_t screenH) {
  if (p.vy >= 0) return;
  const float height = boxHeight(p);
  const float head = p.y - height;
  const float prevHead = head - p.vy;
  for (auto& b : boxes) {
    if (b.broken) continue;
    const float overlap = min(p.x + PLAYER_W, static_cast<float>(b.x + b.w)) -
                          max(p.x, static_cast<float>(b.x));
    if (overlap < SUPPORT_OVERLAP) continue;
    const float bottom = b.y + b.h;
    if (head < bottom && prevHead >= bottom - 4) {
      p.y = bottom + height;
      p.vy = 0;
      hitBox(b);
      return;
    }
  }
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t c = first; c <= last; ++c) {
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      // Only a tile's exposed underside counts. Internal seams inside a wall
      // must not stop a bird that is rising beside it.
      if (r == 0 || !blocksJump(c, r) || plainTile(c, r - 1)) continue;
      const float bottom = rowBottom(r, screenH);
      if (head < bottom && prevHead >= bottom - 4) {
        p.y = bottom + height;
        p.vy = 0;
        return;
      }
    }
  }
}


// World y of the top of the screen. The world scrolls vertically for levels
// taller than the screen; every world draw goes through this offset.
int16_t gCameraY = 0;

// True when the bird could stand up where it is: nothing solid (tile or box)
// in the extra height a standing bird takes above a crouching one. When this
// is false the bird is held in its crouch until it leaves from under the block.
bool hasHeadroom(const Player& p, int16_t screenH) {
  const float standTop = p.y - (PLAYER_H - BIRD_SINK);
  const float crouchTop = p.y - (DUCK_H - BIRD_SINK);
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t c = first; c <= last; ++c) {
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      if (!solidTile(c, r)) continue;
      // A standing bird walking into a direction block must not be forced to
      // crouch; only standing up into a down one from below is stopped.
      const int8_t arrow = arrowAt(c, r);
      if (arrow != ARROW_NONE && (!p.ducking || arrow != ARROW_DOWN)) continue;
      if (rowTop(r, screenH) < crouchTop && rowBottom(r, screenH) > standTop) {
        return false;
      }
    }
  }
  for (const auto& b : boxes) {
    if (b.broken) continue;
    if (p.x + PLAYER_W <= b.x || p.x >= b.x + b.w) continue;
    if (b.y < crouchTop && b.y + b.h > standTop) return false;
  }
  return true;
}

void drawScaled(TFT_eSPI& d, int16_t x, int16_t y, int16_t w, int16_t h,
                const uint16_t* pixels, bool flip, int16_t destH,
                int16_t scale) {
  // Nearest-neighbor draw of a w x h sprite, scale wide and destH tall.
  y -= gCameraY;
  if (y + destH <= 0 || y >= d.height()) return;  // off screen
  const int16_t destW = w * scale;
  for (int16_t dy = 0; dy < destH; ++dy) {
    const int16_t sy = dy * h / destH;
    int16_t runStart = -1;
    uint16_t runColor = 0;
    for (int16_t dx = 0; dx <= destW; ++dx) {
      uint16_t color = SFB_TRANSPARENT;
      if (dx < destW) {
        const int16_t sx = flip ? (w - 1 - dx / scale) : dx / scale;
        color = pgm_read_word(&pixels[sy * w + sx]);
      }
      if (runStart >= 0 && color != runColor) {
        d.drawFastHLine(x + runStart, y + dy, dx - runStart, runColor);
        runStart = -1;
      }
      if (runStart < 0 && color != SFB_TRANSPARENT) {
        runStart = dx;
        runColor = color;
      }
    }
  }
}

void drawCloud(TFT_eSPI& d, int16_t x, int16_t y) {
  d.fillCircle(x, y, 10, CLOUD);
  d.fillCircle(x + 12, y - 5, 13, CLOUD);
  d.fillCircle(x + 28, y, 10, CLOUD);
  d.fillRect(x, y, 28, 10, CLOUD);
}

// Dirt / background tiles (the H cells of the map): drawn, never solid.
bool decorTile(int16_t col, int16_t row) {
  if (col < 0 || col >= level->tiles || row < 0 || row >= 32) return false;
  return (level->decor[col] >> row) & 1u;
}

// Draw every tile of one layer with a single flat tile (the game is plain 2D).
void drawLayer(TFT_eSPI& d, bool (*solid)(int16_t, int16_t), const uint16_t* tile,
               int16_t cameraX, int16_t screenH) {
  const int16_t first = max(0, cameraX / TILE);
  const int16_t last = min<int16_t>(level->tiles - 1, (cameraX + d.width()) / TILE);
  for (int16_t col = first; col <= last; ++col) {
    for (int16_t row = 0; row < MAX_ROWS; ++row) {
      if (!solid(col, row)) continue;
      drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16, tile,
                 false, TILE, SCALE);
    }
  }
}

void drawGround(TFT_eSPI& d, int16_t cameraX, int16_t screenH) {
  drawLayer(d, decorTile, sfbDirt, cameraX, screenH);  // dirt / background
  drawLayer(d, plainTile, sfbGrass, cameraX, screenH);

  const uint16_t* const arrowArt[] = {sfbArrowRight, sfbArrowLeft, sfbArrowUp,
                                      sfbArrowDown};
  const int16_t first = max(0, cameraX / TILE);
  const int16_t last = min<int16_t>(level->tiles - 1, (cameraX + d.width()) / TILE);
  for (int16_t col = first; col <= last; ++col) {
    for (int16_t row = 0; row < MAX_ROWS; ++row) {
      const int8_t arrow = arrowAt(col, row);
      if (arrow == ARROW_NONE) continue;
      drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16,
                 arrowArt[arrow], false, TILE, SCALE);
    }
  }
}

void drawBoxesAndEggs(TFT_eSPI& d, int16_t cameraX) {
  const int16_t screenW = d.width();
  // Eggs first: one pushing out of a box is covered by the box art.
  for (const auto& e : eggs) {
    if (e.state == EggState::None) continue;
    const int16_t ex = static_cast<int16_t>(e.x) - cameraX;
    if (ex + EGG_SIZE < 0 || ex > screenW) continue;
    const uint16_t* art = e.blue ? sfbBlueEgg : sfbEgg;
    if (e.state == EggState::Hop) art = sfbEggCracking;
    if (e.state == EggState::Open) art = sfbEggOpen;
    drawScaled(d, ex, static_cast<int16_t>(e.y) - EGG_SIZE, 16, 16, art, false,
               EGG_SIZE, 1);
  }
  for (const auto& b : boxes) {
    if (b.broken) continue;
    const int16_t bx = b.x - cameraX;
    if (bx + b.w < 0 || bx > screenW) continue;
    const uint16_t* art = b.kind == BoxKind::Plain
                              ? sfbBox
                              : (b.kind == BoxKind::Nest
                                     ? sfbNest
                                     : (b.used ? sfbMysteryOff : sfbMystery));
    drawScaled(d, bx, b.y - b.bump, b.w, b.h, art, false, b.h, 1);
  }
}

void drawGoldEggs(TFT_eSPI& d, int16_t cameraX, int16_t screenH) {
  for (uint8_t i = 0; i < level->goldCount; ++i) {
    if (goldTaken[i]) continue;
    const int16_t x = level->golds[i][0] * TILE - cameraX;
    if (x + EGG_SIZE < 0 || x > d.width()) continue;
    drawScaled(d, x, rowTop(level->golds[i][1], screenH), 16, 16, sfbGoldEgg,
               false, EGG_SIZE, 1);
  }
}

void drawHeart(TFT_eSPI& d, int16_t x, int16_t y, uint16_t color) {
  d.fillCircle(x + 3, y + 3, 3, color);
  d.fillCircle(x + 8, y + 3, 3, color);
  d.fillTriangle(x, y + 4, x + 11, y + 4, x + 5, y + 11, color);
}

struct Hud {
  uint32_t exitHeldMs;
  uint32_t jumpLeftMs;
  uint32_t speedLeftMs;
  int hp;
  bool blink;  // hide the bird this frame (just hurt)
  bool gliding;
  float sink;  // how far the bird is drawn below its feet line right now
  uint32_t levelMs;  // time on this level
  uint32_t totalMs;  // time since the first level
};

// m:ss.t, e.g. 1:05.3
void formatTime(char* out, size_t size, uint32_t ms) {
  const uint32_t tenths = ms / 100;
  snprintf(out, size, "%lu:%02lu.%lu", static_cast<unsigned long>(tenths / 600),
           static_cast<unsigned long>((tenths / 10) % 60),
           static_cast<unsigned long>(tenths % 10));
}

// Terrain is drawn with the bird's feet sunk into the grass, but boxes are
// flat 2D art, so the bird stands right on top of them without the sink.
float targetSink(const Player& p, int16_t screenH) {
  const float landing = landingY(p, screenH);
  const float box = boxSurface(p.x, PLAYER_W, p.y, SUPPORT_OVERLAP);
  return (landing >= 0 && box < 1e8f && fabsf(box - landing) < 0.5f)
             ? 0.0f
             : static_cast<float>(BIRD_SINK);
}

void drawFrame(TFT_eSPI& d, const Player& p, const Gorilla* gorillas,
               int16_t cameraX, int16_t cameraY, int16_t screenH,
               const Hud& hud) {
  gCameraY = cameraY;
  d.fillRect(0, 0, d.width(), d.height(), SKY);

  // Clouds drift at half the camera speed and wrap around.
  constexpr int16_t CLOUD_SPAN = 640;
  for (int16_t i = 0; i < 4; ++i) {
    const int16_t cx =
        ((i * 160 + 20 - cameraX / 2) % CLOUD_SPAN + CLOUD_SPAN) % CLOUD_SPAN - 80;
    drawCloud(d, cx, 30 + (i * 37) % 70);
  }

  drawGround(d, cameraX, screenH);
  drawBoxesAndEggs(d, cameraX);
  drawGoldEggs(d, cameraX, screenH);

  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    const Gorilla& g = gorillas[i];
    if (!g.alive) continue;
    const int16_t gx = static_cast<int16_t>(g.x) - cameraX;
    if (gx + GORILLA_W < 0 || gx > d.width()) continue;
    drawScaled(d, gx, static_cast<int16_t>(g.y) - GORILLA_H + GORILLA_SINK,
               GORILLA_W, GORILLA_H, g.red ? sfbRedGorilla : sfbGorilla, false,
               GORILLA_H, 1);
  }

  for (const auto& r : rats) {
    if (!r.active) continue;
    const int16_t rx = static_cast<int16_t>(r.x) - cameraX;
    if (rx + RAT_W < 0 || rx > d.width()) continue;
    drawScaled(d, rx, static_cast<int16_t>(r.y) - RAT_H + RAT_SINK, 24, 12, sfbRat,
               !r.facingRight, RAT_H, 1);  // the art faces right
  }

  for (const auto& f : foxes) {
    if (!f.active) continue;
    const int16_t fx = static_cast<int16_t>(f.x) - cameraX;
    if (fx + FOX_W < 0 || fx > d.width()) continue;
    // Crouch (squash) while winding up; the art faces right.
    const int16_t h = f.state == FoxState::Windup ? FOX_H - 6 : FOX_H;
    drawScaled(d, fx, static_cast<int16_t>(f.y) - h + FOX_SINK, FOX_W, FOX_H, sfbFox,
               !f.facingRight, h, 1);
  }

  // Shadow on the surface below the bird while airborne; it shrinks as the
  // bird gets higher so the landing spot is easy to judge.
  if (!p.onGround) {
    const float sy = landingY(p, screenH);
    if (sy >= 0) {
      const float height = max(0.0f, sy - p.y);
      const int16_t rx = max<int16_t>(
          4, static_cast<int16_t>(PLAYER_W / 2 - height / 12));
      const int16_t cx = static_cast<int16_t>(p.x + PLAYER_W / 2) - cameraX;
      // The bird's feet are drawn BIRD_SINK below its surface; match that.
      d.fillEllipse(cx, static_cast<int16_t>(sy + targetSink(p, screenH)) - 1 - gCameraY, rx, 3,
                    0x2104);
    }
  }

  // Walk cycle: left foot forward/up/forward/post/post2, then the same for the right foot (stand is the 16 px
  // StandStill art, the walk frames are 32 px wide).
  const uint16_t* const walk[WALK_FRAME_COUNT] = {
      sfbWalk1, sfbWalk0, sfbWalk4, sfbWalk5,   // left foot: up, forward, post, post 2
      sfbWalk3, sfbWalk2, sfbWalk6, sfbWalk7};  // right foot: up, forward, post, post 2
  const uint16_t* art = sfbBird;
  int16_t artW = 32;
  int16_t artH = 32;
  if (p.ducking || hud.gliding) {
    art = sfbDuck;  // crouching on the ground or in the air, and floating
    artW = sfbDuckWidth;
    artH = sfbDuckHeight;
  } else if (!p.onGround) {
    art = sfbWalk1;  // foot tucked up while airborne
  } else if (fabsf(p.vx) > 0.3f) {
    art = walk[static_cast<int>(p.stride) % WALK_FRAME_COUNT];
  } else {
    artW = 16;  // StandStill
  }
  if (!hud.blink) {
    // Drawn at full size; only the crouch hitbox (DUCK_H) is one block tall.
    const int16_t destH = artH * BIRD_SCALE;
    drawScaled(d, static_cast<int16_t>(p.x) - (artW - 16) * BIRD_SCALE / 2 - cameraX,
               static_cast<int16_t>(p.y + hud.sink) - destH, artW, artH, art,
               !p.facingRight, destH, BIRD_SCALE);
  }

  // Hearts (top right) and powerup timers (top left).
  for (int i = 0; i < PLAYER_MAX_HP; ++i) {
    drawHeart(d, d.width() - 14 * (PLAYER_MAX_HP - i) - 2, 4,
              i < hud.hp ? TFT_RED : 0x4208);
  }
  // Golden eggs collected, under the hearts.
  if (level->goldCount > 0) {
    char eggText[12];
    snprintf(eggText, sizeof(eggText), "%d/%d", goldCount, level->goldCount);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TR_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(eggText, d.width() - 4, 22);
    // drawScaled takes world y, so add the camera offset back.
    drawScaled(d, d.width() - 22 - d.textWidth(eggText), 18 + gCameraY, 16, 16,
               sfbGoldEgg, false, EGG_SIZE, 1);
  }
  int16_t barY = 4;
  if (hud.jumpLeftMs > 0) {
    d.fillRect(4, barY, 42, 8, TFT_BLACK);
    d.drawRect(4, barY, 42, 8, TFT_WHITE);
    d.fillRect(6, barY + 2, 38 * hud.jumpLeftMs / BOOST_MS, 4, TFT_RED);
    barY += 10;
  }
  if (hud.speedLeftMs > 0) {
    d.fillRect(4, barY, 42, 8, TFT_BLACK);
    d.drawRect(4, barY, 42, 8, TFT_WHITE);
    d.fillRect(6, barY + 2, 38 * hud.speedLeftMs / BOOST_MS, 4, TFT_CYAN);
  }

  // Timers (top center): this level, and the whole run.
  {
    char level[16], total[16], text[40];
    formatTime(level, sizeof(level), hud.levelMs);
    formatTime(total, sizeof(total), hud.totalMs);
    snprintf(text, sizeof(text), "Lvl %s  Total %s", level, total);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TC_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    const int16_t w = d.textWidth(text) + 6;
    d.fillRect(d.width() / 2 - w / 2, 2, w, 12, TFT_BLACK);
    d.drawString(text, d.width() / 2, 4);
  }

  const uint32_t exitHeldMs = hud.exitHeldMs;
  if (exitHeldMs >= EXIT_SHOW_MS) {
    const int16_t w = d.width() - 40;
    d.fillRoundRect(20, 8, w, 22, 4, TFT_BLACK);
    d.drawRoundRect(20, 8, w, 22, 4, TFT_WHITE);
    d.fillRect(24, 22, (w - 8) * min<uint32_t>(exitHeldMs, EXIT_HOLD_MS) /
                           EXIT_HOLD_MS, 4, TFT_YELLOW);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.setTextDatum(TC_DATUM);
    d.drawString("Keep holding BTN3 to exit", d.width() / 2, 12);
  }
}

void initGorillas(Gorilla* g, int16_t screenH) {
  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    if (i >= level->gorillaCount) {
      g[i] = {0, 0, 0, 0, 1, false, false, 0, 0};
      continue;
    }
    const int16_t col = level->gorillas[i][0];
    const int16_t row = level->gorillas[i][1];
    const float x = col * TILE + (TILE - GORILLA_W) / 2.0f;
    const float y = static_cast<float>(spawnY(row, screenH));
    g[i] = {x, y, 0, x, static_cast<int8_t>(i % 2 == 0 ? 1 : -1),
            level->gorillas[i][2] != 0, true, y, row};
  }
}

void updateGorillas(Gorilla* g, Player& p, int16_t screenH) {
  const float playerX = p.x + PLAYER_W / 2.0f;
  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    Gorilla& a = g[i];
    if (!a.alive) continue;
    float nextX = a.x;
    if (a.red) {
      const float dx = playerX - (a.x + GORILLA_W / 2.0f);
      if (dx > CHASE_SPEED) {
        nextX += CHASE_SPEED;
      } else if (dx < -CHASE_SPEED) {
        nextX -= CHASE_SPEED;
      }
    } else {
      nextX += a.dir * PATROL_SPEED;
      if (nextX > a.homeX + PATROL_RANGE) a.dir = -1;
      if (nextX < a.homeX - PATROL_RANGE) a.dir = 1;
    }
    // Gorillas never step into a pit or up onto a platform: both feet-edges
    // must stay over ground at their home level.
    const int16_t leftCol = columnAt(nextX);
    const int16_t rightCol = columnAt(nextX + GORILLA_W - 0.01f);
    if (solidTile(leftCol, a.homeRow - 1) && solidTile(rightCol, a.homeRow - 1) &&
        !solidTile(leftCol, a.homeRow) && !solidTile(rightCol, a.homeRow)) {
      a.x = nextX;
    } else if (!a.red) {
      a.dir = -a.dir;
    }

    if (a.y >= a.groundY && a.vy >= 0 && random(GORILLA_HOP_ONE_IN) == 0) {
      a.vy = GORILLA_HOP_VELOCITY;
    }
    a.vy += GRAVITY;
    a.y += a.vy;
    if (a.y >= a.groundY) {
      a.y = a.groundY;
      a.vy = 0;
    }

    // Stomp: landing on a gorilla's head from above kills it and bounces.
    const float feet = p.y + BIRD_SINK;
    const float headTop = a.y - GORILLA_H + GORILLA_SINK;
    const bool overlapX = p.x + PLAYER_W > a.x && p.x < a.x + GORILLA_W;
    if (overlapX && p.vy > 0 && feet >= headTop &&
        feet - p.vy <= headTop + 8) {
      a.alive = false;
      p.vy = STOMP_BOUNCE;
      p.onGround = false;
      audioPlayTone(990, 30);
      continue;
    }

    // Any other bump pushes the player away from the gorilla.
    const float playerTop = feet - (p.ducking ? DUCK_H : PLAYER_H);
    const float gorillaBottom = a.y + GORILLA_SINK;
    if (overlapX && feet > headTop && playerTop < gorillaBottom) {
      const bool pushRight =
          p.x + PLAYER_W / 2.0f >= a.x + GORILLA_W / 2.0f;
      const float beforePush = p.x;
      p.x = pushRight ? a.x + GORILLA_W : a.x - PLAYER_W;
      p.vx = pushRight ? PUSH_SPEED : -PUSH_SPEED;
      p.running = false;
      p.x = constrain(p.x, 0.0f, static_cast<float>(worldW() - PLAYER_W));
      resolveWalls(p, beforePush, screenH);
    }
  }
}

void initObjects(int16_t screenH) {
  for (auto& taken : goldTaken) taken = false;
  goldCount = 0;
  for (uint8_t i = 0; i < BOX_SLOTS; ++i) {
    eggs[i] = {EggState::None, 0, 0, 0, 0, 0, false, false};
    rats[i] = {false, 0, 0, 0, true};
    if (i < level->boxCount) {
      const int16_t* spawn = level->boxes[i];
      boxes[i] = {spawn[0], static_cast<int16_t>(screenH - spawn[1] - BOX_SIZE),
                  BOX_SIZE, BOX_SIZE,
                  spawn[2] ? BoxKind::Mystery : BoxKind::Plain, false, false, 0};
    } else {
      boxes[i] = {0, 0, BOX_SIZE, BOX_SIZE, BoxKind::Plain, true, false, 0};
    }
  }
  // Nests: bottom on the ground, centered over their column.
  for (uint8_t i = 0; i < level->nestCount; ++i) {
    const int16_t col = level->nests[i][0];
    const int16_t row = level->nests[i][1];
    boxes[BOX_MAX + i] = {
        static_cast<int16_t>(col * TILE + (TILE - NEST_W) / 2),
        static_cast<int16_t>(spawnY(row, screenH) - NEST_H + NEST_SINK),
        NEST_W, NEST_H,
        BoxKind::Nest, false, false, 0};
  }
  for (uint8_t i = 0; i < FOX_MAX; ++i) {
    if (i >= level->foxCount) {
      foxes[i] = {false, 0, 0, 0, 0, FoxState::Idle, 0, false};
      continue;
    }
    const int16_t foxCol = level->foxes[i][0];
    foxes[i] = {true, foxCol * TILE + (TILE - FOX_W) / 2.0f,
                static_cast<float>(spawnY(level->foxes[i][1], screenH)), 0, 0,
                FoxState::Idle, 0, false};
  }
}

void hurtPlayer(Player& p, Status& st, uint32_t now, float fromX) {
  if (now < st.invulnUntil) return;
  --st.hp;
  st.invulnUntil = now + INVULN_MS;
  const bool pushRight = p.x + PLAYER_W / 2.0f >= fromX;
  p.vx = pushRight ? HURT_KNOCKBACK : -HURT_KNOCKBACK;
  p.vy = HURT_POP;
  p.onGround = false;
  p.running = false;
  audioPlayTone(180, 120);
}

// The bird touching an enemy that is w x h with its feet at (x, y): stomping
// from above kills it (returns true), anything else bites the bird.
bool touchEnemy(Player& p, Status& st, uint32_t now, float x, float y,
                float w, float h) {
  const float top = y - h;
  const float pTop = p.y - boxHeight(p);
  const float pBottom = p.y + BIRD_SINK;
  const bool overlapX = p.x + PLAYER_W > x && p.x < x + w;
  if (!overlapX || pBottom < top || pTop > y) return false;
  if (p.vy > 0 && pBottom - p.vy <= top + 8) {
    p.vy = STOMP_BOUNCE;
    p.onGround = false;
    audioPlayTone(990, 30);
    return true;
  }
  hurtPlayer(p, st, now, x + w / 2.0f);
  return false;
}

bool updateObjects(Player& p, Status& st, uint32_t now, int16_t screenH) {
  const float playerX = p.x + PLAYER_W / 2.0f;
  const float pTop = p.y - boxHeight(p);
  const float pBottom = p.y + BIRD_SINK;  // the feet are drawn this far down
  bool atNest = false;
  for (auto& b : boxes) {
    if (b.bump > 0) --b.bump;
    // Standing on the nest (feet on its top, mostly over it) ends the level.
    if (b.kind == BoxKind::Nest && p.onGround && fabsf(p.y - b.y) < 1.5f &&
        min(p.x + PLAYER_W, static_cast<float>(b.x + b.w)) -
                max(p.x, static_cast<float>(b.x)) >= 8.0f) {
      atNest = true;
    }
  }

  for (uint8_t i = 0; i < level->goldCount; ++i) {
    if (goldTaken[i]) continue;
    const float x = level->golds[i][0] * TILE;
    const float top = rowTop(level->golds[i][1], screenH);
    if (p.x + PLAYER_W > x && p.x < x + EGG_SIZE && pBottom > top &&
        pTop < top + EGG_SIZE) {
      goldTaken[i] = true;
      ++goldCount;
      audioPlayTone(1760, 60);
    }
  }

  for (uint8_t i = 0; i < BOX_SLOTS; ++i) {
    Egg& e = eggs[i];
    const float eggX = e.x + EGG_SIZE / 2.0f;
    switch (e.state) {
      case EggState::None:
        break;
      case EggState::Open:
        if (--e.timer == 0) e.state = EggState::None;
        break;
      case EggState::Emerging:
        e.y -= EGG_EMERGE_SPEED;
        if (e.y <= e.restY) {
          e.y = e.restY;
          e.state = EggState::Idle;
        }
        break;
      case EggState::Idle:
        if (e.rotten) {
          if (fabsf(playerX - eggX) < EGG_TRIGGER_DIST &&
              fabsf(p.y - e.restY) < 6 * TILE) {
            e.vy = EGG_HOP_VELOCITY;
            e.state = EggState::Hop;
            audioPlayTone(520, 30);
          }
        } else if (p.x + PLAYER_W > e.x && p.x < e.x + EGG_SIZE &&
                   pBottom > e.y - EGG_SIZE && pTop < e.y) {
          // Grabbed: the egg is the powerup.
          e.state = EggState::None;
          if (e.blue) {
            st.speedBoostUntil = now + BOOST_MS;
          } else {
            st.jumpBoostUntil = now + BOOST_MS;
          }
          audioPlayTone(1320, 60);
        }
        break;
      case EggState::Hop:
        e.vy += GRAVITY;
        e.y += e.vy;
        if (e.y >= e.restY && e.vy > 0) {
          e.y = e.restY;
          e.state = EggState::Open;
          e.timer = EGG_OPEN_TICKS;
          rats[i] = {true, eggX - RAT_W / 2.0f, e.restY, -3.0f, playerX > eggX};
          audioPlayTone(140, 80);
        }
        break;
    }
  }

  for (auto& r : rats) {
    if (!r.active) continue;
    const float prevY = r.y;
    r.vy = min(r.vy + GRAVITY, MAX_FALL);
    r.y += r.vy;
    const float surface = surfaceBelow(r.x, RAT_W, prevY, 4.0f, screenH);
    if (r.vy >= 0 && surface >= 0 && r.y >= surface) {
      r.y = surface;
      r.vy = 0;
    }
    if (r.y > screenH + FALL_RESET_DEPTH) {
      r.active = false;
      continue;
    }
    // Chase the bird along the ground; walls stop it (it cannot climb).
    const float dx = playerX - (r.x + RAT_W / 2.0f);
    if (fabsf(dx) > RAT_SPEED) {
      const float dir = dx > 0 ? 1.0f : -1.0f;
      r.facingRight = dir > 0;
      const float nextX = r.x + dir * RAT_SPEED;
      const int16_t front = columnAt(dir > 0 ? nextX + RAT_W : nextX);
      const int16_t row =
          static_cast<int16_t>(floorf((screenH - (r.y - 2)) / TILE));
      if (!solidTile(front, row) && nextX >= 0 && nextX + RAT_W <= worldW()) {
        r.x = nextX;
      }
    }

    if (touchEnemy(p, st, now, r.x, r.y, RAT_W, RAT_H)) r.active = false;
  }

  for (auto& f : foxes) {
    if (!f.active) continue;
    const float foxCenter = f.x + FOX_W / 2.0f;
    const float dx = playerX - foxCenter;
    const float dir = dx > 0 ? 1.0f : -1.0f;
    const float prevY = f.y;
    bool airborne = f.state == FoxState::Leap || f.state == FoxState::Pounce;
    if (airborne) {
      f.vy = min(f.vy + GRAVITY, MAX_FALL);
      f.y += f.vy;
    } else {
      f.vy = 0;
    }
    switch (f.state) {
      case FoxState::Idle:
        if (fabsf(dx) < FOX_WAKE_DIST && fabsf(p.y - f.y) < 8 * TILE) {
          f.state = FoxState::Prowl;
        }
        break;
      case FoxState::Prowl: {
        f.facingRight = dir > 0;
        const float nextX = f.x + dir * FOX_RUN_SPEED;
        const int16_t front = columnAt(dir > 0 ? nextX + FOX_W : nextX);
        const int16_t row =
            static_cast<int16_t>(floorf((screenH - (f.y - 2)) / TILE));
        // Races along the ground, but stops at walls and pit edges.
        const float ground = surfaceBelow(nextX, FOX_W, f.y, 4.0f, screenH);
        const bool safe = ground >= 0 && ground - f.y < 4;
        if (!solidTile(front, row) && safe && nextX >= 0 &&
            nextX + FOX_W <= worldW()) {
          f.x = nextX;
        }
        if (fabsf(dx) < FOX_POUNCE_RANGE) {
          f.state = FoxState::Windup;
          f.timer = FOX_WINDUP_TICKS;
        }
        break;
      }
      case FoxState::Windup:
        f.facingRight = dir > 0;
        if (--f.timer == 0) {
          f.state = FoxState::Leap;
          f.vy = FOX_LEAP_VELOCITY;
          audioPlayTone(700, 30);
        }
        break;
      case FoxState::Leap:
        f.facingRight = dir > 0;
        if (f.vy >= 0) {  // top of the leap: pounce forward
          f.state = FoxState::Pounce;
          f.vx = dir * FOX_POUNCE_VX;
          f.vy = FOX_POUNCE_VY;
          audioPlayTone(400, 40);
        }
        break;
      case FoxState::Pounce: {
        const float nextX = f.x + f.vx;
        const int16_t front = columnAt(f.vx > 0 ? nextX + FOX_W : nextX);
        const int16_t row =
            static_cast<int16_t>(floorf((screenH - (f.y - 2)) / TILE));
        if (!solidTile(front, row) && nextX >= 0 && nextX + FOX_W <= worldW()) {
          f.x = nextX;
        } else {
          f.vx = 0;
        }
        break;
      }
      case FoxState::Recover:
        if (--f.timer == 0) f.state = FoxState::Prowl;
        break;
    }

    // Land on whatever is underneath.
    if (airborne) {
      const float surface = surfaceBelow(f.x, FOX_W, prevY, 4.0f, screenH);
      if (f.vy >= 0 && surface >= 0 && f.y >= surface) {
        f.y = surface;
        f.vy = 0;
        f.vx = 0;
        f.state = FoxState::Recover;
        f.timer = FOX_RECOVER_TICKS;
      }
    }
    if (f.y > screenH + FALL_RESET_DEPTH) {
      f.active = false;
      continue;
    }
    if (touchEnemy(p, st, now, f.x, f.y, FOX_W, FOX_H)) f.active = false;
  }
  return atNest;
}

// Big centered message over the game (level complete).
void drawBanner(TFT_eSPI& d, const char* line1, const char* line2,
                const char* line3) {
  const int16_t w = d.width() - 40;
  const int16_t y = d.height() / 2 - 32;
  d.fillRoundRect(20, y, w, 64, 6, TFT_BLACK);
  d.drawRoundRect(20, y, w, 64, 6, TFT_YELLOW);
  d.setTextDatum(MC_DATUM);
  d.setTextFont(2);
  d.setTextSize(1);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString(line1, d.width() / 2, y + 14);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString(line2, d.width() / 2, y + 32);
  d.drawString(line3, d.width() / 2, y + 50);
}

void respawn(Player& p, int16_t screenH) {
  p = {static_cast<float>(level->startCol * TILE),
       static_cast<float>(spawnY(level->startRow, screenH)),
       0, 0, true, true, false, false, 0};
}
}  // namespace

void drawSuperFlashyBirdsIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  d.fillRect(x, y, 80, 58, SKY);
  for (int16_t i = 0; i < 5; ++i) {
    const uint16_t* tile = sfbGrass;
    for (int16_t ty = 0; ty < 16; ++ty) {
      for (int16_t tx = 0; tx < 16; ++tx) {
        const uint16_t c =
            pgm_read_word(&tile[ty * 16 + tx]);
        if (c != SFB_TRANSPARENT) d.drawPixel(x + i * 16 + tx, y + 42 + ty, c);
      }
    }
  }
  for (int16_t ty = 0; ty < 32; ++ty) {
    for (int16_t tx = 0; tx < 16; ++tx) {
      const uint16_t c = pgm_read_word(&sfbBird[ty * 16 + tx]);
      if (c != SFB_TRANSPARENT) d.drawPixel(x + 32 + tx, y + 14 + ty, c);
    }
  }
}

void runSuperFlashyBirds(TFT_eSPI& display) {
  const int16_t screenW = display.width();
  const int16_t screenH = display.height();

  waitForButtonRelease();
  audioBegin();

  TFT_eSprite frame(&display);
  frame.setColorDepth(8);
  frame.createSprite(screenW, screenH);
  const bool useSprite = frame.created();

  int16_t worldH = level->rows * TILE;  // physics "screenH": the world bottom
  float cameraYf = 0;
  bool snapCamera = true;

  Player p;
  respawn(p, worldH);
  Gorilla gorillas[MAX_GORILLAS];
  uint8_t levelIndex = 0;
  level = &LEVELS[levelIndex];
  initGorillas(gorillas, worldH);
  initObjects(worldH);
  Status status = {PLAYER_MAX_HP, 0, 0, 0};
  float visualSink = BIRD_SINK;

  // Double-press detection: when a direction button is pressed again within
  // DOUBLE_PRESS_MS of its last press, running turns on until it is released.
  Button lastDirection = Button::None;
  uint32_t lastDirectionPress = 0;
  uint32_t exitHeldSince = 0;
  uint32_t lastDirectionHeld = 0;
  uint32_t jumpBufferUntil = 0;
  bool crouchWanted = false;  // BTN2 toggles it
  uint32_t levelMs = 0;  // time on the current level (running through deaths)
  uint32_t totalMs = 0;  // time across all levels
  uint32_t lastTick = millis();
  bool gliding = false;       // toggled by jump in the air; ends on landing
  uint32_t lastFrame = millis();
  Button pendingDirection = Button::None;
  bool pendingJump = false;
  bool pendingCrouch = false;

  while (true) {
    const uint32_t now = millis();
    const Button sampledEvent = readButtonEvent();
    const Button held = readButton();
    if (sampledEvent == Button::Button1 ||
        sampledEvent == Button::Button4) {
      pendingDirection = sampledEvent;
    } else if (sampledEvent == Button::Button3) {
      pendingJump = true;
    } else if (sampledEvent == Button::Button2) {
      pendingCrouch = true;
    }
    if (now - lastFrame < FRAME_MS) {
      delay(2);
      continue;
    }
    lastFrame = now;

    // Level and total timers (a long stall, like the level-complete banner,
    // does not count).
    const uint32_t dt = min<uint32_t>(now - lastTick, 100);
    lastTick = now;
    levelMs += dt;
    totalMs += dt;

    const Button event = pendingDirection;
    pendingDirection = Button::None;

    if (event == Button::Button1 || event == Button::Button4) {
      if (event == lastDirection && now - lastDirectionPress <= DOUBLE_PRESS_MS) {
        p.running = true;
      }
      lastDirection = event;
      lastDirectionPress = now;
    }

    // Exit: hold BTN3 for EXIT_HOLD_MS.
    uint32_t exitHeld = 0;
    if (held == Button::Button3) {
      if (exitHeldSince == 0) exitHeldSince = now;
      exitHeld = now - exitHeldSince;
      if (exitHeld >= EXIT_HOLD_MS) {
        break;
      }
    } else {
      exitHeldSince = 0;
    }

    const bool wantLeft = held == Button::Button1;
    const bool wantRight = held == Button::Button4;
    const bool wantJump = held == Button::Button3;
    if (wantLeft || wantRight) lastDirectionHeld = now;
    // A jump needs a fresh press (holding BTN3 to exit must not bounce).
    if (pendingJump) {
      jumpBufferUntil = now + JUMP_BUFFER_MS;
      pendingJump = false;
      // Pressing jump again while already in the air toggles the glide.
      if (!p.onGround) gliding = !gliding;
    }
    const bool coasting = now - lastDirectionHeld < MOMENTUM_GRACE_MS;

    if (!wantLeft && !wantRight && p.onGround && !coasting &&
        fabsf(p.vx) <= WALK_SPEED) {
      p.running = false;
    }
    if ((wantLeft && p.vx > 0 && p.onGround) ||
        (wantRight && p.vx < 0 && p.onGround)) {
      p.running = false;  // turning around ends the run
    }

    // BTN2 toggles the crouch (one block tall). If a block is overhead the bird
    // is held crouched even after toggling off, and stands up by itself as soon
    // as it is out from under it.
    if (pendingCrouch) {
      crouchWanted = !crouchWanted;
      pendingCrouch = false;
    }
    p.ducking = crouchWanted || !hasHeadroom(p, worldH);
    if (p.ducking) p.running = false;

    // Horizontal movement (a crouched bird only walks slowly).
    const float speedMul = now < status.speedBoostUntil ? BOOST_SPEED_FACTOR : 1.0f;
    const float maxSpeed =
        p.ducking ? CROUCH_SPEED : (p.running ? RUN_SPEED : WALK_SPEED) * speedMul;
    if (wantLeft || wantRight) {
      const float dir = wantLeft ? -1.0f : 1.0f;
      p.facingRight = !wantLeft;
      const float accel = p.onGround ? ACCEL : AIR_ACCEL;
      p.vx += dir * accel;
      if (fabsf(p.vx) > maxSpeed) {
        p.vx = dir * maxSpeed;
      }
    } else if (p.onGround && !coasting) {
      p.vx = p.vx > 0 ? max(0.0f, p.vx - GROUND_FRICTION)
                      : min(0.0f, p.vx + GROUND_FRICTION);
    }  // no friction in the air: momentum carries the jump
    if (p.ducking && p.onGround && fabsf(p.vx) > maxSpeed) {
      // Crouching while moving fast: slow down to the crouch walking speed.
      p.vx = p.vx > 0 ? max(maxSpeed, p.vx - GROUND_FRICTION)
                      : min(-maxSpeed, p.vx + GROUND_FRICTION);
    }

    // Jump (variable height) and gravity.
    if (p.onGround && now < jumpBufferUntil) {
      jumpBufferUntil = 0;
      p.vy = now < status.jumpBoostUntil ? JUMP_VELOCITY * BOOST_JUMP_FACTOR
                                          : JUMP_VELOCITY;
      p.onGround = false;
      audioPlayTone(660, 25);
    }
    if (!wantJump && p.vy < JUMP_CUT_VELOCITY) {
      p.vy = JUMP_CUT_VELOCITY;
    }
    // Gliding cuts gravity (and the fall speed) while the bird is falling.
    p.vy += (gliding && p.vy >= 0) ? GRAVITY * GLIDE_GRAVITY_FACTOR : GRAVITY;
    const float maxFall = gliding ? MAX_FALL * GLIDE_MAX_FALL_FACTOR : MAX_FALL;
    if (p.vy > maxFall) p.vy = maxFall;

    const float prevX = p.x;
    // Gliding covers more ground; vx itself is untouched, so the extra speed
    // ends as soon as the glide does.
    p.x += gliding ? p.vx * GLIDE_SPEED_FACTOR : p.vx;
    if (p.onGround && !p.ducking && fabsf(p.vx) > 0.3f) {
      p.stride += p.running ? RUN_FRAMES_PER_TICK : WALK_FRAMES_PER_TICK;
    } else if (p.onGround) {
      p.stride = 0;  // start every walk from the first frame
    }
    p.y += p.vy;

    // Keep the player inside the world's left and right bounds in the air.
    if (p.x < 0) { p.x = 0; p.vx = 0; }
    if (p.x + PLAYER_W > worldW()) { p.x = worldW() - PLAYER_W; p.vx = 0; }

    // Land on a surface under the bird's center (a pit has none), bump the
    // head on tile undersides, and block the sides of solid tiles.
    p.onGround = landOnSurface(p, worldH);
    resolveWalls(p, prevX, worldH);
    bumpHead(p, worldH);
    unstick(p, worldH);
    if (p.onGround) gliding = false;
    const bool fell = p.y > worldH + FALL_RESET_DEPTH;
    if (fell) audioPlayTone(220, 120);

    updateGorillas(gorillas, p, worldH);
    const bool levelDone = updateObjects(p, status, now, worldH);
    if (fell || status.hp <= 0) {
      respawn(p, worldH);
      initGorillas(gorillas, worldH);
      initObjects(worldH);
      snapCamera = true;
      crouchWanted = false;
      gliding = false;
      status = {PLAYER_MAX_HP, 0, 0, 0};
    }
    // Ease the sink so the bird glides on and off boxes instead of popping.
    const float sinkTarget = targetSink(p, worldH);
    visualSink += constrain(sinkTarget - visualSink, -3.0f, 3.0f);
    const Hud hud = {
        exitHeld,
        now < status.jumpBoostUntil ? status.jumpBoostUntil - now : 0,
        now < status.speedBoostUntil ? status.speedBoostUntil - now : 0,
        status.hp, now < status.invulnUntil && (now / 100) % 2 == 0, gliding,
        visualSink,
        levelMs, totalMs};

    // Camera follows the player, clamped to the world. A world shorter than
    // the screen sits at the bottom of it.
    int16_t cameraX = static_cast<int16_t>(p.x + PLAYER_W / 2 - screenW / 2);
    cameraX = constrain(cameraX, 0, worldW() - screenW);
    const float camLo = min<float>(0, worldH - screenH);
    const float camHi = max<float>(0, worldH - screenH);
    const float wantY = constrain(p.y - screenH * 0.6f, camLo, camHi);
    if (snapCamera) {
      cameraYf = wantY;
      snapCamera = false;
    } else {
      cameraYf += (wantY - cameraYf) * 0.2f;
    }
    const int16_t cameraY = static_cast<int16_t>(cameraYf);

    TFT_eSPI& target = useSprite ? static_cast<TFT_eSPI&>(frame) : display;
    drawFrame(target, p, gorillas, cameraX, cameraY, worldH, hud);
    if (levelDone) {
      char title[40], levelTime[16], totalTime[16], times[40];
      if (level->goldCount > 0) {
        snprintf(title, sizeof(title), "Level %d complete!  Eggs %d/%d",
                 levelIndex + 1, goldCount, level->goldCount);
      } else {
        snprintf(title, sizeof(title), "Level %d complete!", levelIndex + 1);
      }
      formatTime(levelTime, sizeof(levelTime), levelMs);
      formatTime(totalTime, sizeof(totalTime), totalMs);
      snprintf(times, sizeof(times), "Level %s   Total %s", levelTime, totalTime);
      const bool last = levelIndex + 1 == LEVEL_COUNT;
      drawBanner(target, title, times,
                 last ? "All done! Back to level 1" : "Loading next level");
    }
    if (useSprite) frame.pushSprite(0, 0);

    if (levelDone) {
      audioPlayTone(1320, 80);
      delay(1800);
      lastTick = millis();
      levelMs = 0;
      levelIndex = (levelIndex + 1) % LEVEL_COUNT;
      if (levelIndex == 0) totalMs = 0;  // a new run after the last level
      level = &LEVELS[levelIndex];
      worldH = level->rows * TILE;
      snapCamera = true;
      crouchWanted = false;
      gliding = false;
      respawn(p, worldH);
      initGorillas(gorillas, worldH);
      initObjects(worldH);
      status.invulnUntil = 0;
      status.jumpBoostUntil = 0;
      status.speedBoostUntil = 0;
      visualSink = BIRD_SINK;
      jumpBufferUntil = 0;
      lastFrame = millis();
    }
  }

  audioEnd();
  frame.deleteSprite();
  waitForButtonRelease();
}

#include "super_flashy_birds.h"

#include <Arduino.h>

#include <cmath>

#include "audio_helper.h"
#include "button_input.h"
#include "super_flashy_sprites.h"

// Controls (one resistor-ladder ADC, so only one button reads at a time):
//   BTN1 left, BTN4 right, BTN3 jump, BTN2 duck (fast-fall in the air).
//   Double-press BTN1/BTN4 and keep holding it to run.
//   Hold BTN3 for 3 seconds to exit back to the menu.
// Because buttons cannot be held together, horizontal speed carries through a
// jump: releasing a direction keeps the speed for MOMENTUM_GRACE_MS (long enough
// to reach the jump button), and BTN1/BTN4 steer in the air.
namespace {
constexpr int16_t SCALE = 2;  // art pixels -> screen pixels
constexpr int16_t TILE = 16 * SCALE;
// The bird is drawn at its native art resolution (1:1) so it stays crisp and
// is one tile tall; the ground tiles use SCALE.
constexpr int16_t BIRD_SCALE = 1;
// Walk/idle frames are 32x32 art; the body (hitbox) is the 16 px in the middle.
constexpr int16_t PLAYER_W = 16 * BIRD_SCALE;
constexpr int16_t PLAYER_H = 32 * BIRD_SCALE;
constexpr int16_t DUCK_H = 24 * BIRD_SCALE;  // FlashyBirdDucking art is 31x24
constexpr int16_t FRAME_ART = 32 * BIRD_SCALE;
// Walk animation advances at a fixed rate per game tick (30 ms): a whole
// number of ticks per frame keeps every frame on screen equally long.
constexpr float WALK_FRAMES_PER_TICK = 1.0f / 3.0f;  // 90 ms per frame
constexpr float RUN_FRAMES_PER_TICK = 1.0f / 2.0f;   // 60 ms per frame
constexpr int WALK_FRAME_COUNT = 8;
constexpr int16_t BIRD_SINK = 12;  // draw the feet slightly into the grass
constexpr int16_t WORLD_TILES = 72;  // ground length in tiles
constexpr int16_t WORLD_W = WORLD_TILES * TILE;
// Column heights in tiles: 0 = pit, 1 = ground level, 2+ = raised platform.
// The walk-jump clears a 2-tile pit; a 3-tile pit needs a running jump.
constexpr uint8_t TERRAIN[WORLD_TILES] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1,   //  0- 9: start, low step
    1, 1, 0, 0, 1, 1, 1, 1, 2, 2,   // 10-19: 2-tile pit, stairs up
    3, 3, 2, 2, 1, 1, 1, 1, 0, 0,   // 20-29: high step, stairs down, pit
    0, 1, 1, 1, 1, 2, 2, 2, 3, 3,   // 30-39: 3-tile pit (run), platform
    3, 1, 1, 1, 0, 0, 1, 1, 2, 2,   // 40-49: 2-tile pit, step up
    1, 1, 1, 1, 4, 4, 1, 1, 1, 1,   // 50-59: tall wall at 54-55
    2, 1, 1, 1, 1, 1, 1, 1, 1, 1,   // 60-69: step up to the hill
    1, 1};                          // 70-71
// Grass ledges (solid, walkable): {column, row}, row 0 is ground level. A dirt
// "lip" hangs under each one. Cols 51-57 stick out of the tall wall; cols 61-69
// are ledges in front of the hill below, a stairway up and over it.
constexpr int16_t SHELVES[][2] = {
    {51, 2}, {52, 2}, {53, 2}, {56, 2}, {57, 2},          // wall ledges
    {61, 2}, {62, 2}, {63, 3}, {64, 3}, {65, 4}, {66, 4},  // hill ledges up
    {67, 4}, {68, 3}, {69, 3}};                           // ... and down
// Background hill (not solid, drawn behind everything): total height in
// tiles per column starting at HILL_START. The player walks in front of it.
constexpr int16_t HILL_START = 61;
constexpr uint8_t HILL[] = {3, 4, 4, 5, 6, 6, 6, 5, 4, 3};
constexpr int16_t LIP = 10 * SCALE;  // the opaque part of the 16x16 DirtLedge art is 10 rows
constexpr int16_t MAX_ROWS = 8;
constexpr uint16_t SKY = 0x5DFF;
constexpr uint16_t CLOUD = TFT_WHITE;

constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t DOUBLE_PRESS_MS = 500;
constexpr uint32_t MOMENTUM_GRACE_MS = 350;
constexpr uint32_t JUMP_BUFFER_MS = 120;
constexpr uint32_t DUCK_GRACE_MS = 350;  // crouch -> jump keeps the crouch
constexpr float STOMP_BOUNCE = -8.0f;
constexpr float PUSH_SPEED = 3.5f;  // knockback speed when a gorilla bumps the bird
constexpr uint32_t EXIT_HOLD_MS = 3000;
constexpr uint32_t EXIT_SHOW_MS = 1500;

constexpr float GRAVITY = 0.7f;
constexpr float JUMP_VELOCITY = -10.0f;
constexpr float JUMP_CUT_VELOCITY = -4.0f;  // released jump button early
constexpr float MAX_FALL = 14.0f;
constexpr float FAST_FALL_ACCEL = 0.9f;
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
constexpr int16_t GORILLA_SINK = 8;  // feet sit slightly in the grass
constexpr int16_t PATROL_RANGE = 3 * TILE;  // blocks each way from home
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
  bool jumpDuck;  // jumped from a crouch: stay crouched until landing
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
  int16_t homeHeight;  // it stays on columns of this height
};

constexpr uint8_t GORILLA_COUNT = 7;
// {home tile, red?}
constexpr int16_t GORILLA_SPAWNS[GORILLA_COUNT][2] = {
    {10, 1}, {15, 0}, {25, 1}, {33, 0}, {52, 0}, {58, 1}, {67, 0}};

int16_t groundTop(int16_t screenH) { return screenH - TILE; }

int16_t colHeight(int16_t col) {
  return (col < 0 || col >= WORLD_TILES) ? 0 : TERRAIN[col];
}

// Y of the top surface of a column (the screen bottom for a pit).
int16_t surfaceY(int16_t col, int16_t screenH) {
  return screenH - colHeight(col) * TILE;
}

int16_t columnAt(float worldX) {
  return static_cast<int16_t>(floorf(worldX / TILE));
}

bool solidTile(int16_t col, int16_t row) {
  if (col < 0 || col >= WORLD_TILES || row < 0) return false;
  if (row < TERRAIN[col]) return true;
  for (const auto& shelf : SHELVES) {
    if (shelf[0] == col && shelf[1] == row) return true;
  }
  return false;
}

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
      if (prevX + PLAYER_W <= c * TILE + 0.01f) {
        p.x = c * TILE - PLAYER_W;  // came from the left
        p.vx = 0;
      } else if (prevX >= (c + 1) * TILE - 0.01f) {
        p.x = (c + 1) * TILE;  // came from the right
        p.vx = 0;
      }
      break;
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
      if (!solidTile(col, r) || solidTile(col, r + 1)) continue;
      const float surface = rowTop(r, screenH);
      if (surface >= prevFeet - 4 && surface < best) best = surface;
    }
  }
  if (best < 1e8f && p.y >= best) {
    p.y = best;
    p.vy = 0;
    return true;
  }
  return false;
}

// Y of the highest surface at or below the feet that the bird would land on,
// or a negative value if it would fall into a pit.
float landingY(const Player& p, int16_t screenH) {
  float best = 1e9f;
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t col = first; col <= last; ++col) {
    if (columnOverlap(p, col) < SUPPORT_OVERLAP) continue;
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      if (!solidTile(col, r) || solidTile(col, r + 1)) continue;
      const float surface = rowTop(r, screenH);
      if (surface >= p.y - 4 && surface < best) best = surface;
    }
  }
  return best < 1e8f ? best : -1.0f;
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
      if (!solidTile(c, r)) continue;
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

// Bump the head on the underside of a tile while rising.
void bumpHead(Player& p, int16_t screenH) {
  if (p.vy >= 0) return;
  const float height = boxHeight(p);
  const float head = p.y - height;
  const float prevHead = head - p.vy;
  const int16_t first = columnAt(p.x);
  const int16_t last = columnAt(p.x + PLAYER_W - 0.01f);
  for (int16_t c = first; c <= last; ++c) {
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      // Only a tile's exposed underside counts. Internal seams inside a wall
      // must not stop a bird that is rising beside it.
      if (r == 0 || !solidTile(c, r) || solidTile(c, r - 1)) continue;
      const float bottom = rowBottom(r, screenH) + LIP;  // includes the lip
      if (head < bottom && prevHead >= bottom - 4) {
        p.y = bottom + height;
        p.vy = 0;
        return;
      }
    }
  }
}


void drawScaled(TFT_eSPI& d, int16_t x, int16_t y, int16_t w, int16_t h,
                const uint16_t* pixels, bool flip, int16_t destH,
                int16_t scale) {
  // Nearest-neighbor draw of a w x h sprite, scale wide and destH tall.
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

int16_t hillHeight(int16_t col) {
  const int16_t i = col - HILL_START;
  return (i < 0 || i >= static_cast<int16_t>(sizeof(HILL))) ? 0 : HILL[i];
}

// Background hill tiles sit above the ground row.
bool hillTile(int16_t col, int16_t row) {
  return row >= 1 && row < hillHeight(col);
}

bool isGrassTile(bool (*solid)(int16_t, int16_t), int16_t col, int16_t row) {
  return solid(col, row) && !solid(col, row + 1);
}

// Draw one layer of tiles. Edge art is picked from what touches each tile:
// open sides get the edge grass/dirt, and dirt that touches a grass tile
// sideways (a cliff step) also gets the edge dirt.
void drawLayer(TFT_eSPI& d, bool (*solid)(int16_t, int16_t), int16_t cameraX,
               int16_t screenH, bool lips) {
  const int16_t first = max(0, cameraX / TILE);
  const int16_t last = min<int16_t>(WORLD_TILES - 1, (cameraX + d.width()) / TILE);
  for (int16_t col = first; col <= last; ++col) {
    for (int16_t row = 0; row < MAX_ROWS; ++row) {
      if (!solid(col, row)) continue;
      const bool openLeft = !solid(col - 1, row);
      const bool openRight = !solid(col + 1, row);
      const bool grassTop = !solid(col, row + 1);
      const uint16_t* tile;
      if (grassTop) {
        if (openLeft != openRight) {
          tile = openLeft ? sfbGrassLeft : sfbGrassRight;
        } else {
          tile = sfbGrass;
        }
      } else {
        const bool edgeLeft = openLeft || isGrassTile(solid, col - 1, row);
        const bool edgeRight = openRight || isGrassTile(solid, col + 1, row);
        if (edgeLeft != edgeRight) {
          tile = edgeLeft ? sfbDirtLeft : sfbDirtRight;
        } else {
          tile = sfbDirt;
        }
      }
      drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16, tile,
                 false, TILE, SCALE);
      // Where a tile hangs over open space, dirt meets the grass underneath.
      if (lips && row >= 1 && !solid(col, row - 1)) {
        drawScaled(d, col * TILE - cameraX, rowBottom(row, screenH), 16, 16,
                   sfbDirtLedge, false, TILE, SCALE);
      }
    }
  }
}

void drawGround(TFT_eSPI& d, int16_t cameraX, int16_t screenH) {
  drawLayer(d, hillTile, cameraX, screenH, false);
  drawLayer(d, solidTile, cameraX, screenH, true);
}

void drawFrame(TFT_eSPI& d, const Player& p, const Gorilla* gorillas,
               int16_t cameraX, int16_t screenH, uint32_t exitHeldMs) {
  d.fillRect(0, 0, d.width(), d.height(), SKY);

  // Clouds drift at half the camera speed and wrap around.
  constexpr int16_t CLOUD_SPAN = 640;
  for (int16_t i = 0; i < 4; ++i) {
    const int16_t cx =
        ((i * 160 + 20 - cameraX / 2) % CLOUD_SPAN + CLOUD_SPAN) % CLOUD_SPAN - 80;
    drawCloud(d, cx, 30 + (i * 37) % 70);
  }

  drawGround(d, cameraX, screenH);

  for (uint8_t i = 0; i < GORILLA_COUNT; ++i) {
    const Gorilla& g = gorillas[i];
    if (!g.alive) continue;
    const int16_t gx = static_cast<int16_t>(g.x) - cameraX;
    if (gx + GORILLA_W < 0 || gx > d.width()) continue;
    drawScaled(d, gx, static_cast<int16_t>(g.y) - GORILLA_H + GORILLA_SINK,
               GORILLA_W, GORILLA_H, g.red ? sfbRedGorilla : sfbGorilla, false,
               GORILLA_H, 1);
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
      d.fillEllipse(cx, static_cast<int16_t>(sy) - 1, rx, 3, 0x2104);
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
  if (p.ducking) {
    art = sfbDuck;  // used on the ground and in the air
    artW = 31;
    artH = 24;
  } else if (!p.onGround) {
    art = sfbWalk1;  // foot tucked up while airborne
  } else if (fabsf(p.vx) > 0.3f) {
    art = walk[static_cast<int>(p.stride) % WALK_FRAME_COUNT];
  } else {
    artW = 16;  // StandStill
  }
  drawScaled(d, static_cast<int16_t>(p.x) - (artW - 16) * BIRD_SCALE / 2 - cameraX,
             static_cast<int16_t>(p.y) - artH * BIRD_SCALE + BIRD_SINK, artW,
             artH, art, !p.facingRight, artH * BIRD_SCALE, BIRD_SCALE);

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
  for (uint8_t i = 0; i < GORILLA_COUNT; ++i) {
    const int16_t col = GORILLA_SPAWNS[i][0];
    const float x = col * TILE + (TILE - GORILLA_W) / 2.0f;
    g[i] = {x, static_cast<float>(surfaceY(col, screenH)), 0, x,
            static_cast<int8_t>(i % 2 == 0 ? 1 : -1), GORILLA_SPAWNS[i][1] != 0,
            true, static_cast<float>(surfaceY(col, screenH)), colHeight(col)};
  }
}

void updateGorillas(Gorilla* g, Player& p, int16_t screenH) {
  const float playerX = p.x + PLAYER_W / 2.0f;
  for (uint8_t i = 0; i < GORILLA_COUNT; ++i) {
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
    // must stay over columns of their home height.
    const int16_t leftCol = columnAt(nextX);
    const int16_t rightCol = columnAt(nextX + GORILLA_W - 0.01f);
    if (colHeight(leftCol) == a.homeHeight &&
        colHeight(rightCol) == a.homeHeight &&
        !solidTile(leftCol, a.homeHeight) &&
        !solidTile(rightCol, a.homeHeight)) {
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
      p.x = constrain(p.x, 0.0f, static_cast<float>(WORLD_W - PLAYER_W));
      resolveWalls(p, beforePush, screenH);
    }
  }
}

void respawn(Player& p, int16_t screenH) {
  p = {static_cast<float>(TILE * 2), static_cast<float>(groundTop(screenH)),
       0, 0, true, true, false, false, 0, false};
}
}  // namespace

void drawSuperFlashyBirdsIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  d.fillRect(x, y, 80, 58, SKY);
  for (int16_t i = 0; i < 5; ++i) {
    const uint16_t* tile = i == 0 ? sfbGrassLeft : (i == 4 ? sfbGrassRight : sfbGrass);
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

  Player p;
  respawn(p, screenH);
  Gorilla gorillas[GORILLA_COUNT];
  initGorillas(gorillas, screenH);

  // Double-press detection: when a direction button is pressed again within
  // DOUBLE_PRESS_MS of its last press, running turns on until it is released.
  Button lastDirection = Button::None;
  uint32_t lastDirectionPress = 0;
  uint32_t exitHeldSince = 0;
  uint32_t lastDirectionHeld = 0;
  uint32_t jumpBufferUntil = 0;
  uint32_t lastDuckHeld = millis() - DUCK_GRACE_MS;
  uint32_t lastFrame = millis();
  Button pendingDirection = Button::None;
  bool pendingJump = false;

  while (true) {
    const uint32_t now = millis();
    const Button sampledEvent = readButtonEvent();
    const Button held = readButton();
    if (sampledEvent == Button::Button1 ||
        sampledEvent == Button::Button4) {
      pendingDirection = sampledEvent;
    } else if (sampledEvent == Button::Button3) {
      pendingJump = true;
    }
    if (now - lastFrame < FRAME_MS) {
      delay(2);
      continue;
    }
    lastFrame = now;

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
    const bool wantDuck = held == Button::Button2;
    const bool wantJump = held == Button::Button3;
    if (wantLeft || wantRight) lastDirectionHeld = now;
    // A jump needs a fresh press (holding BTN3 to exit must not bounce).
    if (pendingJump) {
      jumpBufferUntil = now + JUMP_BUFFER_MS;
      pendingJump = false;
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

    // Ducking works on the ground (stops walking) and in the air (tucks the
    // bird and fast-falls; momentum and steering are unaffected).
    if (wantDuck) lastDuckHeld = now;
    p.ducking = wantDuck || p.jumpDuck;

    // Horizontal movement.
    const float maxSpeed = p.running ? RUN_SPEED : WALK_SPEED;
    if (p.ducking && p.onGround) {
      p.vx = p.vx > 0 ? max(0.0f, p.vx - GROUND_FRICTION)
                      : min(0.0f, p.vx + GROUND_FRICTION);
    } else if (wantLeft || wantRight) {
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

    // Jump (variable height) and gravity.
    if (p.onGround && now < jumpBufferUntil) {
      jumpBufferUntil = 0;
      if (now - lastDuckHeld < DUCK_GRACE_MS) p.jumpDuck = true;
      p.vy = JUMP_VELOCITY;
      p.onGround = false;
      audioPlayTone(660, 25);
    }
    if (!wantJump && p.vy < JUMP_CUT_VELOCITY) {
      p.vy = JUMP_CUT_VELOCITY;
    }
    p.vy += GRAVITY;
    if (wantDuck && !p.onGround) p.vy += FAST_FALL_ACCEL;
    if (p.vy > MAX_FALL) p.vy = MAX_FALL;

    const float prevX = p.x;
    p.x += p.vx;
    if (p.onGround && !p.ducking && fabsf(p.vx) > 0.3f) {
      p.stride += p.running ? RUN_FRAMES_PER_TICK : WALK_FRAMES_PER_TICK;
    } else if (p.onGround) {
      p.stride = 0;  // start every walk from the first frame
    }
    p.y += p.vy;

    // Keep the player inside the world's left and right bounds in the air.
    if (p.x < 0) { p.x = 0; p.vx = 0; }
    if (p.x + PLAYER_W > WORLD_W) { p.x = WORLD_W - PLAYER_W; p.vx = 0; }

    // Land on a surface under the bird's center (a pit has none), bump the
    // head on tile undersides, and block the sides of solid tiles.
    p.onGround = landOnSurface(p, screenH);
    resolveWalls(p, prevX, screenH);
    bumpHead(p, screenH);
    unstick(p, screenH);
    if (p.onGround) p.jumpDuck = false;
    if (p.y > screenH + FALL_RESET_DEPTH) {
      audioPlayTone(220, 120);
      respawn(p, screenH);
      initGorillas(gorillas, screenH);
    }

    updateGorillas(gorillas, p, screenH);

    // Camera follows the player, clamped to the world.
    int16_t cameraX = static_cast<int16_t>(p.x + PLAYER_W / 2 - screenW / 2);
    cameraX = constrain(cameraX, 0, WORLD_W - screenW);

    if (useSprite) {
      drawFrame(frame, p, gorillas, cameraX, screenH, exitHeld);
      frame.pushSprite(0, 0);
    } else {
      drawFrame(display, p, gorillas, cameraX, screenH, exitHeld);
    }
  }

  audioEnd();
  frame.deleteSprite();
  waitForButtonRelease();
}

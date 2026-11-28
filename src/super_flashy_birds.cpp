#include "super_flashy_birds.h"

#include <Arduino.h>

#include <cmath>
#include <cstdlib>

#include "audio/audio_helper.h"
#include "game_assets.h"
#include "input/button_input.h"
#include "super_flashy_art.h"
#include "super_flashy_levels.h"

// Controls (one resistor-ladder ADC, so only one button reads at a time):
//   BTN1 left, BTN4 right, BTN3 jump, BTN2 duck (fast-fall in the air).
//   Double-press BTN1/BTN4 and keep holding it to run, or just keep holding
//   it for a second.
//   Hold BTN3 for 3 seconds to exit back to the menu.
//   Carrying a box or ball: hold BTN2 briefly and let go to throw it, or for a
//   long time and let go to set it down.
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
constexpr uint16_t BOX_MAX = LEVEL_MAX_BOXES;
// The slots after the boxes hold the nests.
constexpr uint16_t BOX_SLOTS = BOX_MAX + LEVEL_MAX_NESTS;
// Only boxes that give something out need room for an egg, rat or bean.
constexpr uint16_t SPECIAL_SLOTS = LEVEL_MAX_SPECIALS;
constexpr uint8_t FOX_MAX = LEVEL_MAX_FOXES;
constexpr uint8_t FROG_MAX = LEVEL_MAX_FROGS;

const Level* level = &LEVELS[0];

int16_t worldW() { return level->tiles * TILE; }

constexpr uint16_t SKY = 0x5DFF;

constexpr uint32_t FRAME_MS = 30;
constexpr uint32_t DOUBLE_PRESS_MS = 750;
constexpr uint32_t RUN_HOLD_MS = 1000;  // holding a direction starts a run
constexpr uint32_t MOMENTUM_GRACE_MS = 350;
constexpr uint32_t JUMP_BUFFER_MS = 120;
constexpr float CROUCH_SPEED = 2.4f;  // walk while crouched
constexpr float STOMP_BOUNCE = -8.0f;
// Jump pressed as the bird lands on an enemy: about twice a normal jump.
constexpr float STOMP_BOOST_VELOCITY = -15.0f;
constexpr uint32_t STOMP_BOOST_MS = 250;  // how late the press may come
// How far below an enemy's head the feet may have been and still stomp it.
constexpr float STOMP_REACH = 10.0f;
constexpr float PUSH_SPEED = 7.0f;  // knockback speed when a gorilla bumps the bird
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
constexpr int GORILLA_HOP_ONE_IN = 60;  // per tick (~2 s on average)
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
  uint8_t stun;     // ticks left stunned by a thrown box
  float knockVx;    // slide from that hit
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
// One-way tiles: they stop the bird only when it moves against their arrow (a
// right block stops leftward movement, an up block stops falling) and let it
// through the other three ways. To enemies they are ordinary tiles. The order
// matches the masks in Level::arrows.
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

// Yellow grass: collides like an up block, and a crouching bird drops through.
bool yellowTile(int16_t col, int16_t row) {
  if (col < 0 || col >= level->tiles || row < 0 || row >= 32) return false;
  return (level->yellow[col] >> row) & 1u;
}

// A solid tile that is not a direction block.
bool plainTile(int16_t col, int16_t row) {
  return solidTile(col, row) && arrowAt(col, row) == ARROW_NONE;
}

// True if the tile stops a bird that is moving in direction `moving`.
bool blocksBird(int16_t col, int16_t row, ArrowDir moving) {
  if (!solidTile(col, row)) return false;
  const int8_t arrow = arrowAt(col, row);
  if (arrow == ARROW_NONE) return true;
  constexpr ArrowDir OPPOSITE[] = {ARROW_LEFT, ARROW_RIGHT, ARROW_DOWN,
                                   ARROW_UP};
  return moving == OPPOSITE[arrow];
}

// True if the tile stops the head of a rising bird.
bool blocksJump(int16_t col, int16_t row) {
  return blocksBird(col, row, ARROW_UP);
}

// ---- Boxes, eggs and rats ---------------------------------------------------
// Boxes are half a tile (16 px) and float freely, so they use rectangle
// collision instead of the tile grid. Plain boxes break when the bird bumps
// them from below. Mystery boxes stay, darken and push an egg out of the top.
// Eggs: red refills the hearts, blue makes the bird faster for a while and
// green gives an extra life; now and then one is
// rotten: it hops and cracks open when the bird gets close, and a rat comes
// out that chases the bird and bites.
// Bean is a mystery box that holds a magic bean instead of an egg. Coin is a
// brick that gives up one coin, then darkens and stays instead of breaking.
enum class BoxKind : uint8_t { Plain, Mystery, Bean, Nest, Coin };

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
  uint16_t slot;  // its place in eggs / beans (not used by plain bricks)
};

enum class EggState : uint8_t { None, Emerging, Idle, Hop, Open };

// Red refills hearts, blue is a speed boost, green is an extra life. A coin
// comes out of a coin brick and behaves like an egg that is never rotten.
enum class EggKind : uint8_t { Red, Blue, Green, Coin };

struct Egg {
  EggState state;
  float x;  // world x of the left side
  float y;  // world y of the bottom
  float vy;
  float restY;  // top of the box it sits on
  uint8_t timer;
  bool rotten;  // cracks open into a rat instead of being a powerup
  EggKind kind;
};

constexpr int16_t RAT_W = 30;  // super/rat.rgb565
constexpr int16_t RAT_H = 19;
constexpr int16_t RAT_SINK = 0;

struct Rat {
  bool active;
  float x;  // world x of the left side
  float y;  // world y of the feet
  float vy;
  bool facingRight;
  uint8_t stun;
  float knockVx;
  bool asleep;  // placed in the map: waits until the bird comes near
};

// Glow frogs puff their throats while they sit. The nearer the bird, the less
// they wait between hops, straight up and down. Now and then they dart a block
// or two to one side along the ground, and later back again.
constexpr int16_t FROG_W = 22;  // the sitting art; the other two are bigger
constexpr int16_t FROG_H = 20;
constexpr float FROG_HOP_VELOCITY = -8.2f;     // about 44 px high
constexpr float FROG_DASH_SPEED = 8.0f;  // sideways, half a block a tick
constexpr int FROG_STROLL_ONE_IN = 100;  // per tick left alone (~3 s)
constexpr int FROG_SIDE_ONE_IN = 3;      // of its moves with the bird near
constexpr float FROG_NOTICE_DIST = 9 * TILE;   // starts hopping this close
constexpr float FROG_FRANTIC_DIST = 2 * TILE;  // hops fastest this close
constexpr uint8_t FROG_LONG_WAIT = 50;         // ticks sat between hops, far
constexpr uint8_t FROG_SHORT_WAIT = 3;         // and near
// After landing a jump it sits this long (2 s) before that wait even starts.
constexpr uint8_t FROG_JUMP_COOLDOWN = 2000 / 30;  // ticks of FRAME_MS
constexpr uint32_t FROG_PUFF_MS = 1200;

struct Frog {
  bool active;
  float x;  // world x of the left side
  float y;  // world y of the feet
  float vy;
  bool airborne;
  bool facingRight;
  uint8_t wait;  // ticks sat since the last landing
  uint8_t stun;
  float knockVx;
  float homeX;    // where its sideways dashes start from and return to
  float targetX;  // where this dash is heading
  float dashVx;   // 0 unless darting sideways
  uint8_t cooldown;  // ticks still to sit after a jump
};

// Timed effects and health for the bird.
struct Status {
  int hp;
  uint32_t invulnUntil;
  uint32_t speedBoostUntil;
  int lives;
};

constexpr int16_t EGG_SIZE = 16;
constexpr float EGG_EMERGE_SPEED = 2.0f;
constexpr float EGG_HOP_VELOCITY = -5.5f;
constexpr float EGG_TRIGGER_DIST = 44.0f;  // a rotten egg wakes at this distance
constexpr uint8_t EGG_OPEN_TICKS = 25;  // the broken shell stays this long
constexpr int ROTTEN_ONE_IN = 5;        // chance an egg is rotten
constexpr uint32_t BOOST_MS = 20000;  // speed boost
constexpr float BOOST_SPEED_FACTOR = 1.3f;
constexpr float RAT_SPEED = 1.5f;
constexpr int PLAYER_MAX_HP = 3;
constexpr int START_LIVES = 10;
constexpr uint32_t INVULN_MS = 1500;
constexpr float HURT_KNOCKBACK = 3.5f;
constexpr float HURT_POP = -5.0f;

constexpr int16_t NEST_W = 40;  // super/nest.rgb565
constexpr int16_t NEST_H = 23;
constexpr int16_t NEST_SINK = 0;

Box boxes[BOX_SLOTS];  // slots from BOX_MAX on are nests (goals)
// eggs[box.slot] came out of that box; the slots after the boxes' hold the eggs
// and rats placed in the map.
Egg eggs[SPECIAL_SLOTS];
Rat rats[SPECIAL_SLOTS];  // rats[i] hatched from eggs[i]

// Foxes: very quick. They sleep until the bird is near, race at it, crouch,
// leap high, and at the top of the leap pounce forward.
enum class FoxState : uint8_t { Idle, Prowl, Windup, Leap, Pounce, Recover };

constexpr int16_t FOX_W = 38;  // super/fox.rgb565
constexpr int16_t FOX_H = 20;
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
  uint8_t stun;
  float knockVx;
};

Fox foxes[FOX_MAX];
Frog frogs[FROG_MAX];

constexpr uint8_t STUN_TICKS = 70;  // about 2 s
constexpr float STUN_KNOCK_SPEED = 4.5f;
constexpr float STUN_KNOCK_FRICTION = 0.3f;

void slowKnock(float& vx) {
  vx = vx > 0 ? max(0.0f, vx - STUN_KNOCK_FRICTION)
              : min(0.0f, vx + STUN_KNOCK_FRICTION);
}

// Golden eggs: collectibles that hang where the map puts them. Dying restarts
// the level, so they all come back.
bool goldTaken[LEVEL_MAX_GOLDS];
uint8_t goldCount = 0;  // collected on this attempt
// The main map's count while a sublevel is being played.
uint8_t goldsFromMain = 0;
// Infinity eggs work the same, but how many there are is never shown.
bool infinityTaken[LEVEL_MAX_INFINITIES];
uint8_t infinityCount = 0;
// Coins are counted the same way.
bool coinTaken[LEVEL_MAX_COINS];
uint16_t coinCount = 0;
// Green eggs placed in the map: each one is an extra life.
bool greenTaken[LEVEL_MAX_GREENS];

// ---- Magic beans and vines --------------------------------------------------
// A bean box pushes out a bean. Walking into it sprouts a vine up to the top of
// the level; touching it while crouched picks it up instead. A carried bean
// is set down by holding crouch, and travels with the bird between areas.
enum class BeanState : uint8_t { None, Emerging, Free, Carried, Vine };

struct Bean {
  BeanState state;
  int16_t col;      // beans and vines stay on the tile grid
  float y;          // world y of the bottom (the vine's foot)
  float vy;
  float restY;      // top of the box it is coming out of
  bool armed;       // false until the bird stops touching a bean it handled
  float top;        // how high the vine has grown
};

// beans[box.slot] came out of that box; the extra slots are for beans carried
// in from another area.
constexpr uint16_t BEAN_SLOTS = SPECIAL_SLOTS + 4;
Bean beans[BEAN_SLOTS];

constexpr float VINE_GROW_SPEED = 6.0f;
constexpr float CLIMB_SPEED = 2.5f;
constexpr uint32_t BEAN_DROP_HOLD_MS = 1000;  // hold crouch to set it down

// ---- Cardboard boxes and tennis balls ---------------------------------------
// Loose items. Walking into one slides it; touching it while crouched picks it
// up. A held crouch throws it or sets it down. A thrown box knocks an enemy
// back and stuns it; a thrown ball kills it.
enum class ItemKind : uint8_t { Cardboard, Ball };
enum class ItemState : uint8_t { None, Free, Carried };

struct Item {
  ItemState state;
  ItemKind kind;
  float x;  // world x of the left side
  float y;  // world y of the bottom
  float vx;
  float vy;
  bool thrown;  // hurts enemies until it comes to rest
  bool armed;   // false until the bird stops touching an item it handled
  float angle;  // how far a ball has turned, in radians (clockwise)
};

// The extra slots are for items carried in from another area.
constexpr uint8_t ITEM_SLOTS = LEVEL_MAX_ITEMS + 4;
Item items[ITEM_SLOTS];

constexpr int16_t CARDBOARD_SIZE = 16;
constexpr int16_t BALL_SIZE = 12;
constexpr float ITEM_SLIDE_SPEED = 3.5f;  // at least this fast when walked into
constexpr float ITEM_FRICTION = 0.25f;
constexpr float ITEM_BOUNCE = 0.3f;  // speed kept after hitting something
constexpr float THROW_SPEED = 7.0f;
constexpr float BALL_THROW_SPEED = 11.0f;
// Fastest spin per tick; any quicker and it seems to turn backwards.
constexpr float BALL_MAX_SPIN = 0.7f;
constexpr float THROW_LIFT = -4.0f;
constexpr uint32_t THROW_HOLD_MS = 300;  // shorter is an ordinary crouch press
constexpr uint32_t ITEM_DROP_HOLD_MS = 1500;  // this long sets it down instead

int16_t itemSize(const Item& it) {
  return it.kind == ItemKind::Ball ? BALL_SIZE : CARDBOARD_SIZE;
}

// The last box the bird bumped this tick: enemies standing on it are knocked off.
const Box* bumpedBox = nullptr;

// An enemy that was knocked off: it tumbles away upside down and cannot hurt.
struct Corpse {
  bool active;
  const uint16_t* art;
  int16_t w;
  int16_t h;
  float x;  // world x of the left side
  float y;  // world y of the bottom
  float vx;
  float vy;
};

constexpr uint8_t MAX_CORPSES = 8;
constexpr float KNOCK_SPEED = 3.0f;
constexpr float KNOCK_POP = -6.0f;
Corpse corpses[MAX_CORPSES];

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
      if (p.vy > 0 && !blocksBird(c, r, ARROW_DOWN) &&
          bottom - p.vy <= rowTop(r, screenH) + 1.0f) {
        continue;  // dropping into a direction block from above
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

// A cardboard box that is sitting still enough to be stood on.
bool standable(const Item& it) {
  return it.state == ItemState::Free && it.kind == ItemKind::Cardboard &&
         !it.thrown && it.vy == 0;
}

// Top of the highest intact box, cardboard ones included, under a span (at or
// below `fromY`), or 1e9.
float boxSurface(float x, float w, float fromY, float minOverlap) {
  float best = 1e9f;
  for (const auto& b : boxes) {
    if (b.broken) continue;
    const float overlap = min(x + w, static_cast<float>(b.x + b.w)) -
                          max(x, static_cast<float>(b.x));
    if (overlap < minOverlap) continue;
    if (b.y >= fromY - 4 && b.y < best) best = b.y;
  }
  for (const auto& it : items) {
    if (!standable(it)) continue;
    const float overlap = min(x + w, it.x + CARDBOARD_SIZE) - max(x, it.x);
    if (overlap < minOverlap) continue;
    const float top = it.y - CARDBOARD_SIZE;
    if (top >= fromY - 4 && top < best) best = top;
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
      if (p.ducking && yellowTile(col, r)) continue;  // drops through
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
                   int16_t screenH, bool forBird = false,
                   bool skipYellow = false) {
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
      if (!top || (skipYellow && yellowTile(col, r))) continue;
      const float surface = rowTop(r, screenH);
      if (surface >= fromY - 4 && surface < best) best = surface;
    }
  }
  best = min(best, boxSurface(x, w, fromY, minOverlap));
  return best < 1e8f ? best : -1.0f;
}

// Where the bird would land (for its shadow); negative over a pit.
float landingY(const Player& p, int16_t screenH) {
  return surfaceBelow(p.x, PLAYER_W, p.y, SUPPORT_OVERLAP, screenH, true,
                      p.ducking);
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

// Bits that fly off a breaking brick, in the brick's own colors.
struct Particle {
  float x;
  float y;
  float vx;
  float vy;
  uint16_t color;
  uint8_t life;  // ticks left; 0 = unused
};

constexpr uint8_t MAX_PARTICLES = 32;
constexpr uint8_t PARTICLES_PER_BRICK = 10;
constexpr uint8_t PARTICLE_LIFE = 18;  // ticks, about half a second
Particle particles[MAX_PARTICLES];

void clearParticles() {
  for (auto& q : particles) q.life = 0;
}

void burstBrick(const Box& b) {
  uint8_t made = 0;
  for (auto& q : particles) {
    if (made == PARTICLES_PER_BRICK) break;
    if (q.life > 0) continue;
    uint16_t color = pgm_read_word(&sfbBox[random(sfbBoxWidth * sfbBoxHeight)]);
    if (color == SFB_TRANSPARENT) color = TFT_BROWN;
    q.x = b.x + random(b.w - 2);
    q.y = b.y + random(b.h - 2);
    q.vx = random(-30, 31) / 10.0f;
    q.vy = random(-50, -9) / 10.0f;
    q.color = color;
    q.life = PARTICLE_LIFE + random(7);
    ++made;
  }
}

void updateParticles() {
  for (auto& q : particles) {
    if (q.life == 0) continue;
    --q.life;
    q.vy += GRAVITY;
    q.x += q.vx;
    q.y += q.vy;
  }
}

// The bird bumped a box from below.
void hitBox(Box& b) {
  if (b.kind == BoxKind::Nest) return;
  bumpedBox = &b;
  if (b.kind == BoxKind::Plain) {
    b.broken = true;
    burstBrick(b);
    audioPlayTone(330, 40);
    return;
  }
  if (b.used) return;
  b.used = true;
  b.bump = 6;
  audioPlayTone(880, 40);
  if (b.kind == BoxKind::Bean) {
    const int16_t col = b.x / TILE;
    beans[b.slot] = {BeanState::Emerging, col, static_cast<float>(b.y + TILE),
                         0, static_cast<float>(b.y), true, 0};
    return;
  }
  Egg& e = eggs[b.slot];
  e.restY = static_cast<float>(b.y);
  e.x = b.x + (b.w - EGG_SIZE) / 2.0f;
  e.y = e.restY + EGG_SIZE;  // starts hidden inside the box
  e.vy = 0;
  e.timer = 0;
  if (b.kind == BoxKind::Coin) {
    e.rotten = false;
    e.kind = EggKind::Coin;
  } else {
    e.rotten = random(ROTTEN_ONE_IN) == 0;
    const long roll = random(5);  // red 40%, blue 40%, green 20%
    e.kind =
        roll < 2 ? EggKind::Red : (roll < 4 ? EggKind::Blue : EggKind::Green);
  }
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
int16_t gClipTop = 0;
int16_t gClipBottom = 32767;

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
                int16_t scale, bool upsideDown = false) {
  // Nearest-neighbor draw of a w x h sprite, scale wide and destH tall.
  y -= gCameraY;
  if (pixels == nullptr) return;
  if (y + destH <= gClipTop || y >= gClipBottom) return;
  if (y + destH <= 0 || y >= d.height()) return;  // off screen
  const int16_t destW = w * scale;
  int16_t firstRow = 0;
  int16_t lastRow = destH;
  if (y < gClipTop) firstRow = gClipTop - y;
  if (y + destH > gClipBottom) lastRow = gClipBottom - y;
  for (int16_t dy = firstRow; dy < lastRow; ++dy) {
    const int16_t sy = upsideDown ? h - 1 - dy * h / destH : dy * h / destH;
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

// Pixels of the 8-bit frame sprite, or null when drawing straight to the screen.
uint8_t* gFrameBuffer = nullptr;

// Unpack one row of the sky (RGB332) into `line`, sfbSkyWidth pixels long.
void skyRow(int16_t y, uint8_t* line) {
  if (sfbSkyRuns == nullptr || sfbSkyRows == nullptr || y < 0 ||
      y >= sfbSkyHeight) {
    memset(line, 0, sfbSkyWidth);
    return;
  }
  const uint8_t* run = sfbSkyRuns + sfbSkyRows[y];
  for (int16_t x = 0; x < sfbSkyWidth; run += 2) {
    const uint8_t length = run[0];
    memset(line + x, run[1], length);
    x += length;
  }
}

// The sky picture scrolled sideways by `offset`, wrapping, into an 8-bit frame
// buffer as wide as the sky.
void copySky(uint8_t* frame, int16_t offset, int16_t rows) {
  uint8_t line[sfbSkyWidth];
  for (int16_t y = 0; y < rows && y < sfbSkyHeight; ++y) {
    skyRow(y, line);
    uint8_t* const target = frame + y * sfbSkyWidth;
    memcpy(target, line + offset, sfbSkyWidth - offset);
    memcpy(target + sfbSkyWidth - offset, line, offset);
  }
}

// In the game the sky scrolls at a quarter of the camera speed. Without a frame
// buffer it is a flat color.
void drawSky(TFT_eSPI& d, int16_t cameraX) {
  if (gFrameBuffer == nullptr || d.width() != sfbSkyWidth) {
    d.fillRect(0, 0, d.width(), d.height(), SKY);
    return;
  }
  copySky(gFrameBuffer, (cameraX / 4) % sfbSkyWidth, d.height());
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

// Which sides of a tile touch another tile of the same kind.
constexpr uint8_t JOIN_UP = 1;
constexpr uint8_t JOIN_DOWN = 2;
constexpr uint8_t JOIN_LEFT = 4;
constexpr uint8_t JOIN_RIGHT = 8;

// Redraw the edges of a grass or cloud tile that join another one, so the two
// read as one piece. Vertically, the fringe rows give way to the body of the
// tile: a diagonal stripe that repeats every 16 px. Horizontally, the darkened
// bottom corners are filled in from the middle of the same row.
// tools/level_editor.py does the same.
void joinTile(const uint16_t* art, uint8_t joins, uint16_t* out) {
  const uint16_t base = pgm_read_word(&art[7 * 16 + 2]);
  const uint16_t stripe = pgm_read_word(&art[7 * 16 + 8]);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint16_t color = pgm_read_word(&art[y * 16 + x]);
      if (((joins & JOIN_UP) && y <= 4) || ((joins & JOIN_DOWN) && y >= 11)) {
        color = (x + y + 3) % 16 < 5 ? stripe : base;
      } else if (y >= 11 && y <= 14 &&
                 (((joins & JOIN_LEFT) && x < 2) ||
                  ((joins & JOIN_RIGHT) && x >= 14))) {
        color = pgm_read_word(&art[y * 16 + (x + 8) % 16]);
      }
      out[y * 16 + x] = color;
    }
  }
}

// Draw the grass (or cloud) tiles of one kind, joined to their own kind.
void drawGrassLayer(TFT_eSPI& d, bool (*same)(int16_t, int16_t),
                    const uint16_t* tile, int16_t cameraX, int16_t screenH) {
  if (tile == nullptr) return;
  const int16_t first = max(0, cameraX / TILE);
  const int16_t last =
      min<int16_t>(level->tiles - 1, (cameraX + d.width()) / TILE);
  uint16_t joined[16 * 16];
  for (int16_t col = first; col <= last; ++col) {
    for (int16_t row = 0; row < MAX_ROWS; ++row) {
      if (!same(col, row)) continue;
      const uint8_t joins = (same(col, row + 1) ? JOIN_UP : 0) |
                            (same(col, row - 1) ? JOIN_DOWN : 0) |
                            (same(col - 1, row) ? JOIN_LEFT : 0) |
                            (same(col + 1, row) ? JOIN_RIGHT : 0);
      if (joins != 0) joinTile(tile, joins, joined);
      drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16,
                 joins != 0 ? joined : tile, false, TILE, SCALE);
    }
  }
}

// True if something other than grass sits on top of a tile: dirt, a direction
// block or an intact box.
bool coveredAbove(int16_t col, int16_t row, int16_t screenH) {
  if (decorTile(col, row + 1) ||
      (arrowAt(col, row + 1) != ARROW_NONE && !yellowTile(col, row + 1))) {
    return true;
  }
  for (const auto& b : boxes) {
    if (!b.broken && b.kind != BoxKind::Nest && b.x == col * TILE &&
        b.y + b.h == rowTop(row, screenH)) {
      return true;
    }
  }
  return false;
}

// True while the level being played is a sublevel, not one of the main maps.
bool inSkyLevel() {
  for (const Level& main : LEVELS) {
    if (&main == level) return false;
  }
  return true;
}

// Grass of either color, for outlines and shading.
bool grassTile(int16_t col, int16_t row) {
  return plainTile(col, row) || yellowTile(col, row);
}

bool boxAt(int16_t col, int16_t row, int16_t screenH);

// Nothing at all is there: the sky shows.
bool skyTile(int16_t col, int16_t row, int16_t screenH) {
  if (col < 0 || col >= level->tiles || row < 0) return false;
  return !solidTile(col, row) && !decorTile(col, row) &&
         !boxAt(col, row, screenH);
}

// Darken a background tile along the edges and corners that meet the sky. If
// grass shades it from above, only what is darker still is drawn.
void drawSkyShade(TFT_eSPI& d, int16_t col, int16_t row, int16_t cameraX,
                  int16_t screenH, const uint16_t* art, bool underGrass) {
  struct Side {
    int8_t dc, dr;
  } sides[8];
  uint8_t count = 0;
  for (int8_t dr = -1; dr <= 1; ++dr) {
    for (int8_t dc = -1; dc <= 1; ++dc) {
      if ((dc != 0 || dr != 0) && skyTile(col + dc, row + dr, screenH)) {
        sides[count++] = {dc, dr};
      }
    }
  }
  if (count == 0 || art == nullptr) return;
  const int16_t left = col * TILE - cameraX;
  const int16_t top = rowTop(row, screenH) - gCameraY;
  for (int16_t py = 0; py < TILE; ++py) {
    const int16_t y = top + py;
    if (y < gClipTop || y >= gClipBottom || y < 0 || y >= d.height()) continue;
    const uint8_t already = underGrass && py < 9 ? (py < 5 ? 2 : 1) : 0;
    for (int16_t px = 0; px < TILE; ++px) {
      int16_t nearest = TILE;
      for (uint8_t i = 0; i < count; ++i) {
        // Rows count upward in the map, so +1 is the top edge on screen.
        const int16_t dx = sides[i].dc < 0   ? px
                           : sides[i].dc > 0 ? TILE - 1 - px
                                             : 0;
        const int16_t dy = sides[i].dr > 0   ? py
                           : sides[i].dr < 0 ? TILE - 1 - py
                                             : 0;
        nearest = min<int16_t>(nearest, max(dx, dy));
      }
      const uint8_t depth = nearest < 3 ? 2 : nearest < 6 ? 1 : 0;
      if (depth <= already) continue;
      const uint16_t color = pgm_read_word(&art[py * TILE + px]);
      if (color == SFB_TRANSPARENT) continue;
      const uint8_t keep = depth == 2 ? 9 : 12;  // sixteenths of the light
      const uint16_t r = ((color >> 11) * keep) >> 4;
      const uint16_t g = (((color >> 5) & 0x3F) * keep) >> 4;
      const uint16_t b = ((color & 0x1F) * keep) >> 4;
      d.drawPixel(left + px, y, (r << 11) | (g << 5) | b);
    }
  }
}

void drawGround(TFT_eSPI& d, int16_t cameraX, int16_t screenH) {
  // In sublevels the X ground is cloud, so is the H background, and the
  // direction blocks are the white ones.
  const bool cloud = inSkyLevel();
  drawLayer(d, decorTile, cloud ? sfbCloudBackground : sfbDirt, cameraX,
            screenH);  // dirt / background
  drawGrassLayer(d, plainTile, cloud ? sfbCloud : sfbGrass, cameraX, screenH);
  drawGrassLayer(d, yellowTile, sfbYellowGrass, cameraX, screenH);

  const uint16_t* const arrowArt[] = {sfbArrowRight, sfbArrowLeft, sfbArrowUp,
                                      sfbArrowDown};
  // Direction blocks connected to grass (directly or through other grassy
  // blocks) are grassy; tools/build_levels.py works out which.
  const uint16_t* const grassyArt[] = {sfbGrassyArrowRight, sfbGrassyArrowLeft,
                                       sfbGrassyArrowUp, sfbGrassyArrowDown};
  const uint16_t* const whiteArt[] = {sfbWhiteArrowRight, sfbWhiteArrowLeft,
                                      sfbWhiteArrowUp, sfbWhiteArrowDown};
  const int16_t first = max(0, cameraX / TILE);
  const int16_t last = min<int16_t>(level->tiles - 1, (cameraX + d.width()) / TILE);
  for (int16_t col = first; col <= last; ++col) {
    for (int16_t row = 0; row < MAX_ROWS; ++row) {
      // Black outline on the sides and bottom of a grass tile that are not
      // touching another grass tile; the top only where something covers it.
      const bool yellow = yellowTile(col, row);
      if (yellow || plainTile(col, row)) {
        if (cloud && !yellow) continue;  // the cloud picture has its own edges
        const int16_t x = col * TILE - cameraX;
        const int16_t y = rowTop(row, screenH) - gCameraY;
        if (coveredAbove(col, row, screenH)) {
          d.drawFastHLine(x, y, TILE, TFT_BLACK);
        }
        if (!grassTile(col, row - 1)) {
          d.drawFastHLine(x, y + TILE - 1, TILE, TFT_BLACK);
        }
        if (!grassTile(col - 1, row)) d.drawFastVLine(x, y, TILE, TFT_BLACK);
        if (!grassTile(col + 1, row)) {
          d.drawFastVLine(x + TILE - 1, y, TILE, TFT_BLACK);
        }
        continue;
      }
      if (decorTile(col, row)) {
        // Grass shades the dirt directly underneath it.
        const bool underGrass = !cloud && grassTile(col, row + 1);
        if (underGrass) {
          drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16,
                     sfbDirtShadow, false, TILE, SCALE);
        }
        drawSkyShade(d, col, row, cameraX, screenH,
                     cloud ? sfbCloudBackground : sfbDirt, underGrass);
        continue;
      }
      const int8_t arrow = arrowAt(col, row);
      if (arrow == ARROW_NONE) continue;
      const bool grassy = (level->grassy[col] >> row) & 1u;
      drawScaled(d, col * TILE - cameraX, rowTop(row, screenH), 16, 16,
                 (cloud ? whiteArt : grassy ? grassyArt : arrowArt)[arrow],
                 false, TILE, SCALE);
    }
  }
}

// A bean with its bottom-left at (x, bottom): x on screen, bottom in the world.
void drawBean(TFT_eSPI& d, int16_t x, int16_t bottom, bool flip = false) {
  drawScaled(d, x, bottom - sfbBeanHeight, sfbBeanWidth, sfbBeanHeight, sfbBean,
             flip, sfbBeanHeight, 1);
}

// A copy of a 16 x 16 picture turned clockwise about its middle.
void turnTile(const uint16_t* art, float angle, uint16_t* out) {
  const float c = cosf(angle);
  const float s = sinf(angle);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      const float dx = x - 7.5f;
      const float dy = y - 7.5f;
      const int sx = static_cast<int>(floorf(c * dx + s * dy + 8.0f));
      const int sy = static_cast<int>(floorf(-s * dx + c * dy + 8.0f));
      out[y * 16 + x] = sx < 0 || sx >= 16 || sy < 0 || sy >= 16
                            ? SFB_TRANSPARENT
                            : pgm_read_word(&art[sy * 16 + sx]);
    }
  }
}

// An item with its bottom-left at (x, bottom): x on screen, bottom in the
// world. The 16 px picture is centered on the item.
void drawItem(TFT_eSPI& d, const Item& it, int16_t x, int16_t bottom) {
  const int16_t s = itemSize(it);
  const uint16_t* art =
      it.kind == ItemKind::Ball ? sfbTennisBall : sfbCardboardBox;
  uint16_t turned[16 * 16];
  if (it.kind == ItemKind::Ball && it.angle != 0) {
    turnTile(art, it.angle, turned);
    art = turned;
  }
  drawScaled(d, x + s / 2 - 8, bottom - s / 2 - 8, 16, 16, art, false, 16, 1);
}

// Little stars over a stunned enemy; `top` is the world y of its head.
void drawStun(TFT_eSPI& d, int16_t centerX, int16_t top) {
  const int16_t y = top - gCameraY - 3;
  const int16_t swing = (millis() / 120) % 2 == 0 ? 0 : 3;
  for (int8_t i = -1; i <= 1; ++i) {
    d.fillRect(centerX + i * 7 + swing - 2, y - (i == 0 ? 2 : 0), 3, 3,
               TFT_YELLOW);
  }
}

// Placeholder art for the vine, until there is a picture for it.
void drawVines(TFT_eSPI& d, int16_t cameraX) {
  constexpr uint16_t STEM = 0x0480;
  for (const auto& n : beans) {
    if (n.state != BeanState::Vine) continue;
    const int16_t x = n.col * TILE - cameraX;
    if (x + TILE < 0 || x > d.width()) continue;
    const int16_t foot = static_cast<int16_t>(n.y) - gCameraY;
    const int16_t tip = static_cast<int16_t>(n.top) - gCameraY;
    for (int16_t y = foot; y > tip; y -= 8) {
      if (y < 0 || y - 8 > d.height()) continue;
      const int16_t part = min<int16_t>(8, y - tip);
      d.fillRect(x + 6, y - part, 4, part, STEM);
      const bool left = ((foot - y) / 8) % 2 == 0;
      d.fillEllipse(x + (left ? 3 : 12), y - 4, 3, 2, TFT_GREEN);
    }
  }
}

// The golden eggs collected so far, sitting in a nest drawn at (x, y). The
// front of the nest is drawn again over them so they sit inside it.
void drawNestEggs(TFT_eSPI& d, int16_t x, int16_t y) {
  const int16_t count = goldCount + (inSkyLevel() ? goldsFromMain : 0);
  if (count == 0 || sfbNest == nullptr) return;
  constexpr int16_t FRONT = 12;  // first nest row in front of the eggs
  const int16_t step = count > 1 ? min<int16_t>(11, 22 / (count - 1)) : 0;
  const int16_t left = x + (NEST_W - EGG_SIZE - step * (count - 1)) / 2;
  for (int16_t i = 0; i < count; ++i) {
    drawScaled(d, left + i * step, y + FRONT - EGG_SIZE + 3, 16, 16, sfbGoldEgg,
               false, EGG_SIZE, 1);
  }
  drawScaled(d, x, y + FRONT, NEST_W, NEST_H - FRONT, sfbNest + FRONT * NEST_W,
             false, NEST_H - FRONT, 1);
}

// A copy of a tile at 40% brightness.
void darkenTile(const uint16_t* art, uint16_t* out) {
  for (int i = 0; i < TILE * TILE; ++i) {
    const uint16_t color = pgm_read_word(&art[i]);
    if (color == SFB_TRANSPARENT) {
      out[i] = color;
      continue;
    }
    const uint16_t r = (color >> 11) * 2 / 5;
    const uint16_t g = ((color >> 5) & 0x3F) * 2 / 5;
    const uint16_t b = (color & 0x1F) * 2 / 5;
    out[i] = (r << 11) | (g << 5) | b;
  }
}

void drawBoxesAndEggs(TFT_eSPI& d, int16_t cameraX) {
  const int16_t screenW = d.width();
  // Eggs first: one pushing out of a box is covered by the box art.
  for (const auto& e : eggs) {
    if (e.state == EggState::None) continue;
    const int16_t ex = static_cast<int16_t>(e.x) - cameraX;
    if (ex + EGG_SIZE < 0 || ex > screenW) continue;
    const uint16_t* art = e.kind == EggKind::Blue    ? sfbBlueEgg
                          : e.kind == EggKind::Green ? sfbGreenEgg
                          : e.kind == EggKind::Coin  ? sfbCoin
                                                     : sfbEgg;
    if (e.state == EggState::Hop) art = sfbEggCracking;
    if (e.state == EggState::Open) art = sfbEggOpen;
    drawScaled(d, ex, static_cast<int16_t>(e.y) - EGG_SIZE, 16, 16, art, false,
               EGG_SIZE, 1);
  }
  for (const auto& n : beans) {
    if (n.state != BeanState::Emerging && n.state != BeanState::Free) continue;
    drawBean(d, n.col * TILE - cameraX, static_cast<int16_t>(n.y));
  }
  for (const auto& b : boxes) {
    if (b.broken) continue;
    const int16_t bx = b.x - cameraX;
    if (bx + b.w < 0 || bx > screenW) continue;
    // A used coin brick is the brick darkened like a used mystery box.
    const bool coinBrick = b.kind == BoxKind::Coin;
    uint16_t dark[TILE * TILE];
    const bool darkened = coinBrick && b.used && sfbBox != nullptr;
    if (darkened) darkenTile(sfbBox, dark);
    const uint16_t* art = darkened ? dark
                          : b.kind == BoxKind::Plain || coinBrick
                              ? sfbBox
                              : (b.kind == BoxKind::Nest
                                     ? sfbNest
                                     : (b.used ? sfbMysteryOff : sfbMystery));
    drawScaled(d, bx, b.y - b.bump, b.w, b.h, art, false, b.h, 1);
    if (b.kind == BoxKind::Nest) drawNestEggs(d, bx, b.y);
  }
}

// A rare egg in its 16 px cell (x on screen, top in the world): it swells and
// shrinks inside a dotted halo that grows and fades with it.
void drawGlowingEgg(TFT_eSPI& d, int16_t x, int16_t top, const uint16_t* art,
                    uint16_t glow) {
  constexpr uint32_t PULSE_MS = 1200;
  constexpr int16_t MAX_SIZE = 19;
  if (art == nullptr) return;
  const float pulse =
      0.5f + 0.5f * sinf((millis() % PULSE_MS) * 2.0f * PI / PULSE_MS);

  const int16_t cx = x + EGG_SIZE / 2;
  const int16_t cy = top + EGG_SIZE / 2 - gCameraY;
  const float radius = 9.0f + 4.0f * pulse;
  const int16_t reach = static_cast<int16_t>(radius) + 2;
  for (int16_t dy = -reach; dy <= reach; ++dy) {
    const int16_t y = cy + dy;
    if (y < gClipTop || y >= gClipBottom || y < 0 || y >= d.height()) continue;
    for (int16_t dx = -reach; dx <= reach; ++dx) {
      // A little taller than wide, like the egg.
      const float far = sqrtf(dx * dx + dy * dy * 0.8f);
      if (far > radius) continue;
      // Half the pixels near the egg, a quarter further out.
      const bool lit = far < radius - 3.0f ? ((cx + dx + y) & 1) == 0
                                           : ((cx + dx) & 1) == 0 && (y & 1) == 0;
      if (lit) d.drawPixel(cx + dx, y, glow);
    }
  }

  const int16_t size = EGG_SIZE - 1 + static_cast<int16_t>(pulse * 4.0f);
  const int16_t n = min(size, MAX_SIZE);
  uint16_t scaled[MAX_SIZE * MAX_SIZE];
  for (int16_t py = 0; py < n; ++py) {
    for (int16_t px = 0; px < n; ++px) {
      scaled[py * n + px] = pgm_read_word(
          &art[(py * EGG_SIZE / n) * EGG_SIZE + px * EGG_SIZE / n]);
    }
  }
  drawScaled(d, cx - n / 2, top + EGG_SIZE / 2 - n / 2, n, n, scaled, false, n,
             1);
}

void drawGoldEggs(TFT_eSPI& d, int16_t cameraX, int16_t screenH) {
  for (uint8_t i = 0; i < level->goldCount; ++i) {
    if (goldTaken[i]) continue;
    const int16_t x = level->golds[i][0] * TILE - cameraX;
    if (x + EGG_SIZE + 8 < 0 || x - 8 > d.width()) continue;
    drawGlowingEgg(d, x, rowTop(level->golds[i][1], screenH), sfbGoldEgg,
                   0xFFE0);
  }
  for (uint8_t i = 0; i < level->infinityCount; ++i) {
    if (infinityTaken[i]) continue;
    const int16_t x = level->infinities[i][0] * TILE - cameraX;
    if (x + EGG_SIZE + 8 < 0 || x - 8 > d.width()) continue;
    drawGlowingEgg(d, x, rowTop(level->infinities[i][1], screenH),
                   sfbInfinityEgg, TFT_WHITE);
  }
  for (uint16_t i = 0; i < level->coinCount; ++i) {
    if (coinTaken[i]) continue;
    const int16_t x = level->coins[i][0] * TILE - cameraX;
    if (x + TILE < 0 || x > d.width()) continue;
    drawScaled(d, x, rowTop(level->coins[i][1], screenH), 16, 16, sfbCoin,
               false, TILE, SCALE);
  }
  for (uint8_t i = 0; i < level->greenCount; ++i) {
    if (greenTaken[i]) continue;
    const int16_t x = level->greens[i][0] * TILE - cameraX;
    if (x + EGG_SIZE < 0 || x > d.width()) continue;
    drawScaled(d, x, rowTop(level->greens[i][1], screenH), 16, 16, sfbGreenEgg,
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
  uint32_t speedLeftMs;
  int hp;
  int lives;
  bool blink;  // hide the bird this frame (just hurt)
  bool gliding;
  bool climbing;
  float sink;  // how far the bird is drawn below its feet line right now
  uint32_t levelMs;  // time on this level
  uint32_t totalMs;  // time since the first level
  bool dropReady;    // letting go of crouch now sets the carried item down
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
  drawSky(d, cameraX);

  drawGround(d, cameraX, screenH);
  drawBoxesAndEggs(d, cameraX);
  drawGoldEggs(d, cameraX, screenH);
  drawVines(d, cameraX);
  for (const auto& it : items) {
    if (it.state != ItemState::Free) continue;
    drawItem(d, it, static_cast<int16_t>(it.x) - cameraX,
             static_cast<int16_t>(it.y));
  }
  for (const auto& q : particles) {
    if (q.life == 0) continue;
    d.fillRect(static_cast<int16_t>(q.x) - cameraX,
               static_cast<int16_t>(q.y) - gCameraY, 3, 3, q.color);
  }

  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    const Gorilla& g = gorillas[i];
    if (!g.alive) continue;
    const int16_t gx = static_cast<int16_t>(g.x) - cameraX;
    if (gx + GORILLA_W < 0 || gx > d.width()) continue;
    drawScaled(d, gx, static_cast<int16_t>(g.y) - GORILLA_H + GORILLA_SINK,
               GORILLA_W, GORILLA_H, g.red ? sfbRedGorilla : sfbGorilla, false,
               GORILLA_H, 1);
    if (g.stun > 0) {
      drawStun(d, gx + GORILLA_W / 2, static_cast<int16_t>(g.y) - GORILLA_H);
    }
  }

  for (const auto& r : rats) {
    if (!r.active) continue;
    const int16_t rx = static_cast<int16_t>(r.x) - cameraX;
    if (rx + RAT_W < 0 || rx > d.width()) continue;
    drawScaled(d, rx, static_cast<int16_t>(r.y) - RAT_H + RAT_SINK, RAT_W, RAT_H, sfbRat,
               !r.facingRight, RAT_H, 1);  // the art faces right
    if (r.stun > 0) {
      drawStun(d, rx + RAT_W / 2, static_cast<int16_t>(r.y) - RAT_H);
    }
  }

  for (const auto& f : foxes) {
    if (!f.active) continue;
    const int16_t fx = static_cast<int16_t>(f.x) - cameraX;
    if (fx + FOX_W < 0 || fx > d.width()) continue;
    // Crouch (squash) while winding up; the art faces right.
    const int16_t h = f.state == FoxState::Windup ? FOX_H - 6 : FOX_H;
    drawScaled(d, fx, static_cast<int16_t>(f.y) - h + FOX_SINK, FOX_W, FOX_H, sfbFox,
               !f.facingRight, h, 1);
    if (f.stun > 0) {
      drawStun(d, fx + FOX_W / 2, static_cast<int16_t>(f.y) - FOX_H);
    }
  }

  for (uint8_t i = 0; i < FROG_MAX; ++i) {
    const Frog& f = frogs[i];
    if (!f.active) continue;
    const int16_t fx = static_cast<int16_t>(f.x) - cameraX;
    if (fx + FROG_W + 3 < 0 || fx - 3 > d.width()) continue;
    // The art faces left. Each frog puffs out of step with the others.
    const uint16_t* art = sfbFrog;
    int16_t w = FROG_W;
    int16_t h = FROG_H;
    int16_t left = fx;
    if (f.airborne || f.dashVx != 0) {
      art = sfbFrogLeaping;
      w = 24;
      h = 25;
      left = fx - 1;
    } else if ((millis() / FROG_PUFF_MS + i) % 2 != 0) {
      art = sfbFrogPuffed;
      w = 25;
      if (!f.facingRight) left = fx + FROG_W - w;  // the throat swells forward
    }
    drawScaled(d, left, static_cast<int16_t>(f.y) - h, w, h, art,
               f.facingRight, h, 1);
    if (f.stun > 0) {
      drawStun(d, fx + FROG_W / 2, static_cast<int16_t>(f.y) - FROG_H);
    }
  }

  for (const auto& c : corpses) {
    if (!c.active) continue;
    const int16_t cx = static_cast<int16_t>(c.x) - cameraX;
    if (cx + c.w < 0 || cx > d.width()) continue;
    drawScaled(d, cx, static_cast<int16_t>(c.y) - c.h, c.w, c.h, c.art,
               c.vx < 0, c.h, 1, true);
  }

  // Shadow on the surface below the bird while airborne; it shrinks as the
  // bird gets higher so the landing spot is easy to judge.
  if (!p.onGround && !hud.climbing) {
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
  if (hud.gliding) {
    art = sfbDive;  // floating
    artW = sfbDiveWidth;
    artH = sfbDiveHeight;
  } else if (p.ducking) {
    art = sfbDuck;  // used on the ground and in the air
    artW = sfbDuckWidth;
    artH = sfbDuckHeight;
  } else if (!p.onGround) {
    art = sfbWalk1;  // foot tucked up while airborne
  } else if (fabsf(p.vx) > 0.3f) {
    art = walk[static_cast<int>(p.stride) % WALK_FRAME_COUNT];
  } else {
    artW = 16;  // StandStill
  }
  for (const auto& it : items) {  // carried: behind the bird's back
    if (it.state != ItemState::Carried || hud.blink || hud.dropReady) continue;
    drawItem(d, it,
             static_cast<int16_t>(p.x) - cameraX + (p.facingRight ? -9 : 9),
             static_cast<int16_t>(p.y) - (p.ducking ? 4 : 9));
  }
  if (!hud.blink) {
    // Drawn at full size; only the crouch hitbox (DUCK_H) is one block tall.
    const int16_t destH = artH * BIRD_SCALE;
    drawScaled(d, static_cast<int16_t>(p.x) - (artW - 16) * BIRD_SCALE / 2 - cameraX,
               static_cast<int16_t>(p.y + hud.sink) - destH, artW, artH, art,
               !p.facingRight, destH, BIRD_SCALE);
  }
  // Ready to be set down: held out in front of the bird instead.
  for (const auto& it : items) {
    if (it.state != ItemState::Carried || hud.blink || !hud.dropReady) continue;
    const int16_t ahead = p.facingRight ? PLAYER_W - 4 : 4 - itemSize(it);
    drawItem(d, it, static_cast<int16_t>(p.x) - cameraX + ahead,
             static_cast<int16_t>(p.y) - (p.ducking ? 0 : 6));
  }
  for (const auto& n : beans) {  // a carried bean rides on the bird's back
    if (n.state != BeanState::Carried || hud.blink) continue;
    drawBean(d,
             static_cast<int16_t>(p.x) - cameraX + (p.facingRight ? -6 : 7),
             static_cast<int16_t>(p.y) - (p.ducking ? 7 : 9), !p.facingRight);
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
  // Infinity eggs: only how many have been found.
  if (infinityCount > 0) {
    char countText[6];
    snprintf(countText, sizeof(countText), "%d", infinityCount);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TR_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(countText, d.width() - 4, 40);
    drawScaled(d, d.width() - 22 - d.textWidth(countText), 36 + gCameraY, 16, 16,
               sfbInfinityEgg, false, EGG_SIZE, 1);
  }
  // Coins collected.
  if (coinCount > 0) {
    char coinText[8];
    snprintf(coinText, sizeof(coinText), "%u", coinCount);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TR_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(coinText, d.width() - 4, 58);
    drawScaled(d, d.width() - 22 - d.textWidth(coinText), 54 + gCameraY, 16, 16,
               sfbCoin, false, 16, 1);
  }
  // Lives left (top left).
  {
    char livesText[16];
    snprintf(livesText, sizeof(livesText), "Lives %d", hud.lives);
    d.setTextFont(1);
    d.setTextSize(1);
    d.setTextDatum(TL_DATUM);
    d.setTextColor(TFT_WHITE, TFT_BLACK);
    d.drawString(livesText, 4, 4);
  }
  const int16_t barY = 16;
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

// True if an intact brick or mystery box fills the cell.
bool boxAt(int16_t col, int16_t row, int16_t screenH) {
  for (const auto& b : boxes) {
    if (!b.broken && b.kind != BoxKind::Nest && b.x == col * TILE &&
        b.y == rowTop(row, screenH)) {
      return true;
    }
  }
  return false;
}

float gBirdPrevY = 0;   // the bird's y before this tick's movement
bool gStomped = false;  // an enemy was stomped this tick

// True if the bird came down this tick onto a head at height `top`. It goes by
// how far the bird moved, not by vy: landing on the ground in the same tick, or
// bouncing off another enemy, changes vy first.
bool cameDownOn(const Player& p, float top) {
  return p.y > gBirdPrevY + 0.01f && gBirdPrevY + BIRD_SINK <= top + STOMP_REACH;
}

void stompBounce(Player& p) {
  p.vy = STOMP_BOUNCE;
  p.onGround = false;
  gStomped = true;
  audioPlayTone(990, 30);
}

void updateGorillas(Gorilla* g, Player& p, int16_t screenH) {
  const float playerX = p.x + PLAYER_W / 2.0f;
  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    Gorilla& a = g[i];
    if (!a.alive) continue;
    const float wasY = a.y;
    float nextX = a.x;
    const bool stunned = a.stun > 0;
    if (stunned) {
      --a.stun;
      nextX = constrain(a.x + a.knockVx, 0.0f,
                        static_cast<float>(worldW() - GORILLA_W));
      slowKnock(a.knockVx);
    } else if (a.red) {
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
    const auto support = [&](int16_t col) {
      return solidTile(col, a.homeRow - 1) ||
             boxAt(col, a.homeRow - 1, screenH);
    };
    if (support(leftCol) && support(rightCol) &&
        !solidTile(leftCol, a.homeRow) && !solidTile(rightCol, a.homeRow) &&
        !boxAt(leftCol, a.homeRow, screenH) &&
        !boxAt(rightCol, a.homeRow, screenH)) {
      a.x = nextX;
    } else if (!a.red && !stunned) {
      a.dir = -a.dir;
    }

    if (!stunned && a.y >= a.groundY && a.vy >= 0 &&
        random(GORILLA_HOP_ONE_IN) == 0) {
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
    // A hopping gorilla may have risen into the bird, so its old head counts.
    const float wasHeadTop = wasY - GORILLA_H + GORILLA_SINK;
    if (overlapX && feet >= headTop &&
        cameDownOn(p, max(headTop, wasHeadTop))) {
      a.alive = false;
      stompBounce(p);
      continue;
    }

    // Any other bump pushes the player away from the gorilla.
    const float playerTop = feet - (p.ducking ? DUCK_H : PLAYER_H);
    const float gorillaBottom = a.y + GORILLA_SINK;
    if (!stunned && overlapX && feet > headTop && playerTop < gorillaBottom) {
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
  clearParticles();
  for (auto& n : beans) n.state = BeanState::None;
  for (auto& it : items) it.state = ItemState::None;
  for (uint8_t i = 0; i < level->itemCount; ++i) {
    const ItemKind kind =
        level->items[i][2] != 0 ? ItemKind::Ball : ItemKind::Cardboard;
    const int16_t size = kind == ItemKind::Ball ? BALL_SIZE : CARDBOARD_SIZE;
    items[i] = {ItemState::Free,
                kind,
                level->items[i][0] * TILE + (TILE - size) / 2.0f,
                static_cast<float>(spawnY(level->items[i][1], screenH)),
                0,
                0,
                false,
                true};
  }
  for (auto& c : corpses) c.active = false;
  bumpedBox = nullptr;
  for (auto& taken : goldTaken) taken = false;
  goldCount = 0;
  for (auto& taken : infinityTaken) taken = false;
  infinityCount = 0;
  for (auto& taken : coinTaken) taken = false;
  coinCount = 0;
  for (auto& taken : greenTaken) taken = false;
  for (uint16_t i = 0; i < SPECIAL_SLOTS; ++i) {
    eggs[i] = {EggState::None, 0, 0, 0, 0, 0, false, EggKind::Red};
    rats[i] = {false, 0, 0, 0, true};
  }
  uint16_t specials = 0;
  for (uint16_t i = 0; i < BOX_SLOTS; ++i) {
    if (i < level->boxCount) {
      const int16_t* spawn = level->boxes[i];
      boxes[i] = {spawn[0], static_cast<int16_t>(screenH - spawn[1] - BOX_SIZE),
                  BOX_SIZE, BOX_SIZE,
                  spawn[2] == 3   ? BoxKind::Coin
                  : spawn[2] == 2 ? BoxKind::Bean
                  : spawn[2]    ? BoxKind::Mystery
                                : BoxKind::Plain,
                  false, false, 0, 0};
      if (boxes[i].kind != BoxKind::Plain) boxes[i].slot = specials++;
    } else {
      boxes[i] = {0, 0, BOX_SIZE, BOX_SIZE, BoxKind::Plain, true, false, 0};
    }
  }
  // Eggs and rats placed in the map take the slots after the boxes'.
  for (uint16_t i = 0; i < level->looseCount; ++i) {
    const int16_t col = level->loose[i][0];
    const float bottom = spawnY(level->loose[i][1], screenH);
    const int16_t type = level->loose[i][2];  // red, blue, rotten, rat
    const uint16_t slot = specials++;
    if (type == 3) {
      rats[slot] = {true, col * TILE + (TILE - RAT_W) / 2.0f, bottom, 0, false};
      rats[slot].asleep = true;
    } else {
      // A rotten egg looks like any red one until the bird comes near.
      eggs[slot] = {EggState::Idle,
                    static_cast<float>(col * TILE),
                    bottom,
                    0,
                    bottom,
                    0,
                    type == 2,
                    type == 1 ? EggKind::Blue : EggKind::Red};
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
  for (uint8_t i = 0; i < FROG_MAX; ++i) {
    if (i >= level->frogCount) {
      frogs[i] = {false, 0, 0, 0, false, false, 0, 0, 0, 0, 0, 0};
      continue;
    }
    const int16_t frogCol = level->frogs[i][0];
    const float frogX = frogCol * TILE + (TILE - FROG_W) / 2.0f;
    frogs[i] = {true,
                frogX,
                static_cast<float>(spawnY(level->frogs[i][1], screenH)),
                0, false, false, 0, 0, 0, frogX, frogX, 0};
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
                float w, float h, bool stunned, float rose = 0) {
  const float top = y - h;
  const float pTop = p.y - boxHeight(p);
  const float pBottom = p.y + BIRD_SINK;
  const bool overlapX = p.x + PLAYER_W > x && p.x < x + w;
  if (!overlapX || pBottom < top || pTop > y) return false;
  // An enemy that rose into the bird is judged by where its head was.
  if (cameDownOn(p, top + rose)) {
    stompBounce(p);
    return true;
  }
  if (!stunned) hurtPlayer(p, st, now, x + w / 2.0f);
  return false;
}

// A stunned rat or fox sliding back; walls and boxes stop it.
void slideKnocked(float& x, float y, float w, float& vx, int16_t screenH) {
  const float nextX = x + vx;
  const int16_t front = columnAt(vx > 0 ? nextX + w : nextX);
  const int16_t row = static_cast<int16_t>(floorf((screenH - (y - 2)) / TILE));
  if (!solidTile(front, row) && !boxAt(front, row, screenH) && nextX >= 0 &&
      nextX + w <= worldW()) {
    x = nextX;
  } else {
    vx = 0;
  }
  slowKnock(vx);
}

// What an egg or coin out of a box does for the player.
void takeEgg(EggKind kind, Status& st, uint32_t now) {
  if (kind == EggKind::Coin) {
    ++coinCount;
  } else if (kind == EggKind::Blue) {
    st.speedBoostUntil = now + BOOST_MS;
  } else if (kind == EggKind::Green) {
    ++st.lives;
  } else {
    st.hp = PLAYER_MAX_HP;
  }
  audioPlayTone(1320, 60);
}

// True if a frog at x could not be there: a wall or box where its body goes.
bool frogBlocked(float x, float y, int16_t screenH) {
  if (x < 0 || x + FROG_W > worldW()) return true;
  for (const float height : {2.0f, FROG_H - 2.0f}) {
    const int16_t row =
        static_cast<int16_t>(floorf((screenH - (y - height)) / TILE));
    for (const float side : {0.0f, FROG_W - 0.01f}) {
      const int16_t col = columnAt(x + side);
      if (solidTile(col, row) || boxAt(col, row, screenH)) return true;
    }
  }
  return false;
}

// Starts a sitting frog darting 1 or 2 blocks sideways, or back home if it is
// away. It only goes where there is level ground and nothing in the way; false
// if there is nowhere to go.
bool frogDash(Frog& f, int16_t screenH) {
  const auto canReach = [&](float to) {
    const float ground = surfaceBelow(to, FROG_W, f.y, 4.0f, screenH);
    if (ground < 0 || ground - f.y >= 4) return false;
    const float step = to > f.x ? 4.0f : -4.0f;
    for (float x = f.x; fabsf(to - x) > 4.0f; x += step) {
      if (frogBlocked(x + step, f.y, screenH)) return false;
    }
    return !frogBlocked(to, f.y, screenH);
  };

  float to = f.homeX;
  bool found = fabsf(f.x - f.homeX) > 2.0f && canReach(to);
  if (!found) {
    const float side = random(2) == 0 ? -1.0f : 1.0f;
    const int blocks = random(1, 3);
    // The picked way and distance first, then what else there is.
    const float tries[] = {side * blocks, -side * blocks, side, -side};
    for (const float t : tries) {
      to = f.x + t * TILE;
      if (canReach(to)) {
        found = true;
        break;
      }
    }
    if (found) f.homeX = f.x;
  }
  if (!found) return false;
  f.targetX = to;
  f.dashVx = to > f.x ? FROG_DASH_SPEED : -FROG_DASH_SPEED;
  f.facingRight = to > f.x;
  return true;
}

void updateFrogs(Player& p, Status& st, uint32_t now, int16_t screenH) {
  const float playerX = p.x + PLAYER_W / 2.0f;
  for (auto& f : frogs) {
    if (!f.active) continue;
    const float dx = playerX - (f.x + FROG_W / 2.0f);
    const float prevY = f.y;
    if (f.stun > 0) {
      --f.stun;
      slideKnocked(f.x, f.y, FROG_W, f.knockVx, screenH);
      f.homeX = f.x;  // knocked somewhere new
      f.dashVx = 0;
    } else if (f.dashVx != 0) {
      const float left = f.targetX - f.x;
      const bool arrives = fabsf(left) <= FROG_DASH_SPEED;
      const float nextX = f.x + (arrives ? left : f.dashVx);
      if (frogBlocked(nextX, f.y, screenH)) {
        f.dashVx = 0;
      } else {
        f.x = nextX;
        if (arrives) f.dashVx = 0;
      }
    }

    // Gravity always, so a frog falls if its brick breaks.
    f.vy = min(f.vy + GRAVITY, MAX_FALL);
    f.y += f.vy;
    if (f.vy < 0) {  // a low ceiling cuts the hop short
      const int16_t row = static_cast<int16_t>(
          floorf((screenH - (f.y - FROG_H)) / TILE));
      const int16_t col = columnAt(f.x + FROG_W / 2.0f);
      if (solidTile(col, row) || boxAt(col, row, screenH)) {
        f.y = prevY;
        f.vy = 0;
      }
    }
    const float surface = surfaceBelow(f.x, FROG_W, prevY, 4.0f, screenH);
    if (f.vy >= 0 && surface >= 0 && f.y >= surface) {
      f.y = surface;
      f.vy = 0;
      f.airborne = false;
    } else {
      f.airborne = true;
      f.wait = 0;
      f.dashVx = 0;
    }
    if (f.y > screenH + FALL_RESET_DEPTH) {
      f.active = false;
      continue;
    }

    const bool sitting = !f.airborne && f.stun == 0 && f.dashVx == 0;
    if (sitting && f.cooldown > 0) {
      --f.cooldown;
      f.facingRight = dx > 0;
    } else if (sitting) {
      f.facingRight = dx > 0;
      bool hop = false;
      bool sideways = false;
      if (fabsf(dx) < FROG_NOTICE_DIST && fabsf(p.y - f.y) < 8 * TILE) {
        // 0 when frantic, 1 at the edge of noticing.
        const float away = constrain(
            (fabsf(dx) - FROG_FRANTIC_DIST) /
                (FROG_NOTICE_DIST - FROG_FRANTIC_DIST),
            0.0f, 1.0f);
        const float pause =
            FROG_SHORT_WAIT + away * (FROG_LONG_WAIT - FROG_SHORT_WAIT);
        if (++f.wait >= pause) {
          hop = true;
          sideways = random(FROG_SIDE_ONE_IN) == 0;
        }
      } else {
        f.wait = 0;
        hop = sideways = random(FROG_STROLL_ONE_IN) == 0;
      }
      if (hop) {
        f.wait = 0;
        const bool dashed = sideways && frogDash(f, screenH);
        // Left alone it only darts about; with the bird near it jumps whenever
        // it is not darting.
        const bool near = fabsf(dx) < FROG_NOTICE_DIST &&
                          fabsf(p.y - f.y) < 8 * TILE;
        if (!dashed && near) {
          f.vy = FROG_HOP_VELOCITY;
          f.airborne = true;
          f.cooldown = FROG_JUMP_COOLDOWN;
        }
      }
    }

    if (touchEnemy(p, st, now, f.x, f.y, FROG_W, FROG_H, f.stun > 0,
                   max(0.0f, prevY - f.y))) {
      f.active = false;
    }
  }
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
  for (uint8_t i = 0; i < level->infinityCount; ++i) {
    if (infinityTaken[i]) continue;
    const float x = level->infinities[i][0] * TILE;
    const float top = rowTop(level->infinities[i][1], screenH);
    if (p.x + PLAYER_W > x && p.x < x + EGG_SIZE && pBottom > top &&
        pTop < top + EGG_SIZE) {
      infinityTaken[i] = true;
      ++infinityCount;
      audioPlayTone(2093, 60);
    }
  }
  for (uint16_t i = 0; i < level->coinCount; ++i) {
    if (coinTaken[i]) continue;
    const float x = level->coins[i][0] * TILE;
    const float top = rowTop(level->coins[i][1], screenH);
    if (p.x + PLAYER_W > x && p.x < x + TILE && pBottom > top &&
        pTop < top + TILE) {
      coinTaken[i] = true;
      ++coinCount;
      audioPlayTone(1568, 30);
    }
  }
  for (uint8_t i = 0; i < level->greenCount; ++i) {
    if (greenTaken[i]) continue;
    const float x = level->greens[i][0] * TILE;
    const float top = rowTop(level->greens[i][1], screenH);
    if (p.x + PLAYER_W > x && p.x < x + EGG_SIZE && pBottom > top &&
        pTop < top + EGG_SIZE) {
      greenTaken[i] = true;
      ++st.lives;
      audioPlayTone(1320, 60);
    }
  }

  for (uint16_t i = 0; i < SPECIAL_SLOTS; ++i) {
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
          // Grabbed.
          e.state = EggState::None;
          takeEgg(e.kind, st, now);
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
    if (r.asleep && (r.stun > 0 || (fabsf(dx) < FOX_WAKE_DIST &&
                                    fabsf(p.y - r.y) < 8 * TILE))) {
      r.asleep = false;
    }
    if (r.stun > 0) {
      --r.stun;
      slideKnocked(r.x, r.y, RAT_W, r.knockVx, screenH);
    } else if (!r.asleep && fabsf(dx) > RAT_SPEED) {
      const float dir = dx > 0 ? 1.0f : -1.0f;
      r.facingRight = dir > 0;
      const float nextX = r.x + dir * RAT_SPEED;
      const int16_t front = columnAt(dir > 0 ? nextX + RAT_W : nextX);
      const int16_t row =
          static_cast<int16_t>(floorf((screenH - (r.y - 2)) / TILE));
      if (!solidTile(front, row) && !boxAt(front, row, screenH) &&
          nextX >= 0 && nextX + RAT_W <= worldW()) {
        r.x = nextX;
      }
    }

    if (touchEnemy(p, st, now, r.x, r.y, RAT_W, RAT_H, r.stun > 0)) {
      r.active = false;
    }
  }

  for (auto& f : foxes) {
    if (!f.active) continue;
    const float foxCenter = f.x + FOX_W / 2.0f;
    const float dx = playerX - foxCenter;
    const float dir = dx > 0 ? 1.0f : -1.0f;
    const float prevY = f.y;
    const bool stunned = f.stun > 0;
    bool airborne = f.state == FoxState::Leap || f.state == FoxState::Pounce;
    if (stunned) {
      // Knocked back: it slides, falls if pushed off a ledge, and then gets up.
      --f.stun;
      slideKnocked(f.x, f.y, FOX_W, f.knockVx, screenH);
      f.vy = min(f.vy + GRAVITY, MAX_FALL);
      f.y += f.vy;
      const float surface = surfaceBelow(f.x, FOX_W, prevY, 4.0f, screenH);
      if (f.vy >= 0 && surface >= 0 && f.y >= surface) {
        f.y = surface;
        f.vy = 0;
      }
      airborne = false;
    } else if (airborne) {
      f.vy = min(f.vy + GRAVITY, MAX_FALL);
      f.y += f.vy;
    } else {
      f.vy = 0;
    }
    if (!stunned) switch (f.state) {
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
        if (!solidTile(front, row) && !boxAt(front, row, screenH) && safe &&
            nextX >= 0 &&
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
        if (!solidTile(front, row) && !boxAt(front, row, screenH) &&
            nextX >= 0 && nextX + FOX_W <= worldW()) {
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
    if (touchEnemy(p, st, now, f.x, f.y, FOX_W, FOX_H, f.stun > 0)) {
      f.active = false;
    }
  }
  updateFrogs(p, st, now, screenH);
  return atNest;
}

// ---- Knocking enemies off bumped boxes --------------------------------------
void knockOff(const uint16_t* art, int16_t w, int16_t h, float x, float y,
              float fromX) {
  const float vx = x + w / 2.0f >= fromX ? KNOCK_SPEED : -KNOCK_SPEED;
  for (auto& c : corpses) {
    if (c.active) continue;
    c = {true, art, w, h, x, y, vx, KNOCK_POP};
    break;
  }
  audioPlayTone(990, 30);
}

// Kill every enemy standing on box b; fromX is the bird that bumped it.
void knockEnemiesOn(const Box& b, Gorilla* gorillas, float fromX) {
  const auto standsOn = [&](float x, float w, float feet) {
    return fabsf(feet - b.y) <= 2.0f &&
           min(x + w, static_cast<float>(b.x + b.w)) -
                   max(x, static_cast<float>(b.x)) >= 2.0f;
  };
  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    Gorilla& g = gorillas[i];
    // groundY, so a gorilla in the middle of a little hop still counts.
    if (!g.alive || !standsOn(g.x, GORILLA_W, g.groundY)) continue;
    g.alive = false;
    knockOff(g.red ? sfbRedGorilla : sfbGorilla, GORILLA_W, GORILLA_H, g.x, g.y,
             fromX);
  }
  for (auto& r : rats) {
    if (!r.active || !standsOn(r.x, RAT_W, r.y)) continue;
    r.active = false;
    knockOff(sfbRat, RAT_W, RAT_H, r.x, r.y, fromX);
  }
  for (auto& f : foxes) {
    if (!f.active || !standsOn(f.x, FOX_W, f.y)) continue;
    f.active = false;
    knockOff(sfbFox, FOX_W, FOX_H, f.x, f.y, fromX);
  }
  for (auto& f : frogs) {
    if (!f.active || !standsOn(f.x, FROG_W, f.y)) continue;
    f.active = false;
    knockOff(sfbFrog, FROG_W, FROG_H, f.x, f.y, fromX);
  }
}

// A bird crawling on the ground breaks any brick it moves into from the side.
// Call before resolveWalls so the brick does not stop it.
void breakBricksAhead(const Player& p, float prevX, Gorilla* gorillas) {
  if (!p.onGround || !p.ducking) return;
  const float top = p.y - boxHeight(p);
  for (auto& b : boxes) {
    const bool brick = b.kind == BoxKind::Plain ||
                       (b.kind == BoxKind::Coin && !b.used);
    if (b.broken || !brick) continue;
    if (b.y >= p.y - 1.0f || b.y + b.h <= top) continue;
    if (p.x + PLAYER_W <= b.x || p.x >= b.x + b.w) continue;
    if (prevX + PLAYER_W > b.x + 0.01f && prevX < b.x + b.w - 0.01f) continue;
    hitBox(b);
    knockEnemiesOn(b, gorillas, p.x + PLAYER_W / 2.0f);
    bumpedBox = nullptr;  // already dealt with
  }
}

void updateCorpses(int16_t screenH) {
  for (auto& c : corpses) {
    if (!c.active) continue;
    c.vy = min(c.vy + GRAVITY, MAX_FALL);
    c.x += c.vx;
    c.y += c.vy;
    if (c.y - c.h > screenH + FALL_RESET_DEPTH) c.active = false;
  }
}

// ---- Items ------------------------------------------------------------------
int carriedBean();

int carriedItem() {
  for (int i = 0; i < ITEM_SLOTS; ++i) {
    if (items[i].state == ItemState::Carried) return i;
  }
  return -1;
}

// True if something s px square at (x, bottom y) would be stopped by a tile or
// box while moving in direction `moving`.
bool itemBlocked(float x, float y, int16_t s, ArrowDir moving,
                 int16_t screenH) {
  if (x < 0 || x + s > worldW()) return true;
  const float top = y - s;
  for (int16_t c = columnAt(x); c <= columnAt(x + s - 0.01f); ++c) {
    for (int16_t r = 0; r < MAX_ROWS; ++r) {
      if (!blocksBird(c, r, moving)) continue;
      if (rowTop(r, screenH) < y - 1.0f && rowBottom(r, screenH) > top) {
        return true;
      }
    }
  }
  for (const auto& b : boxes) {
    if (b.broken) continue;
    if (x + s > b.x && x < b.x + b.w && b.y < y - 1.0f && b.y + b.h > top) {
      return true;
    }
  }
  return false;
}

// The cardboard box, other than `self`, that overlaps something s px square at
// (x, bottom y), or null.
const Item* cardboardAt(float x, float y, int16_t s, const Item* self) {
  for (const auto& o : items) {
    if (&o == self || o.state != ItemState::Free ||
        o.kind != ItemKind::Cardboard) {
      continue;
    }
    if (x + s > o.x && x < o.x + CARDBOARD_SIZE &&
        y - 1.0f > o.y - CARDBOARD_SIZE && y - s < o.y - 1.0f) {
      return &o;
    }
  }
  return nullptr;
}

// A thrown item collects the eggs and coins it flies through. A rotten egg is
// not collected: it wakes and cracks open.
void itemCollects(const Item& it, Status& st, uint32_t now, int16_t screenH) {
  const float s = itemSize(it);
  const auto hits = [&](float x, float top) {
    return it.x + s > x && it.x < x + EGG_SIZE && it.y > top &&
           it.y - s < top + EGG_SIZE;
  };
  // Takes the ones placed in the map that it touches, and says how many.
  const auto sweep = [&](const int16_t(*spots)[2], uint16_t count,
                         bool* taken, uint16_t tone) {
    uint16_t got = 0;
    for (uint16_t i = 0; i < count; ++i) {
      if (taken[i] ||
          !hits(spots[i][0] * TILE, rowTop(spots[i][1], screenH))) {
        continue;
      }
      taken[i] = true;
      ++got;
      audioPlayTone(tone, 40);
    }
    return got;
  };
  goldCount += sweep(level->golds, level->goldCount, goldTaken, 1760);
  infinityCount +=
      sweep(level->infinities, level->infinityCount, infinityTaken, 2093);
  coinCount += sweep(level->coins, level->coinCount, coinTaken, 1568);
  st.lives += sweep(level->greens, level->greenCount, greenTaken, 1320);

  for (auto& e : eggs) {
    if (e.state != EggState::Idle || !hits(e.x, e.y - EGG_SIZE)) continue;
    if (e.rotten) {
      e.vy = EGG_HOP_VELOCITY;
      e.state = EggState::Hop;
      audioPlayTone(520, 30);
    } else {
      e.state = EggState::None;
      takeEgg(e.kind, st, now);
    }
  }
}

// A thrown item meeting an enemy. True if it hit one.
bool itemHitsEnemy(const Item& it, Gorilla* gorillas) {
  const float s = itemSize(it);
  const auto hits = [&](float x, float y, float w, float h) {
    return it.x + s > x && it.x < x + w && it.y > y - h && it.y - s < y;
  };
  const bool ball = it.kind == ItemKind::Ball;
  const float knock = it.vx > 0 ? STUN_KNOCK_SPEED : -STUN_KNOCK_SPEED;
  const float fromX = it.x + s / 2.0f - it.vx * 4;  // where it came from
  for (uint8_t i = 0; i < MAX_GORILLAS; ++i) {
    Gorilla& g = gorillas[i];
    if (!g.alive || !hits(g.x, g.y, GORILLA_W, GORILLA_H)) continue;
    if (ball) {
      g.alive = false;
      knockOff(g.red ? sfbRedGorilla : sfbGorilla, GORILLA_W, GORILLA_H, g.x,
               g.y, fromX);
    } else {
      g.stun = STUN_TICKS;
      g.knockVx = knock;
      audioPlayTone(250, 60);
    }
    return true;
  }
  for (auto& r : rats) {
    if (!r.active || !hits(r.x, r.y, RAT_W, RAT_H)) continue;
    if (ball) {
      r.active = false;
      knockOff(sfbRat, RAT_W, RAT_H, r.x, r.y, fromX);
    } else {
      r.stun = STUN_TICKS;
      r.knockVx = knock;
      audioPlayTone(250, 60);
    }
    return true;
  }
  for (auto& f : foxes) {
    if (!f.active || !hits(f.x, f.y, FOX_W, FOX_H)) continue;
    if (ball) {
      f.active = false;
      knockOff(sfbFox, FOX_W, FOX_H, f.x, f.y, fromX);
    } else {
      f.stun = STUN_TICKS;
      f.knockVx = knock;
      f.vx = 0;
      f.vy = 0;
      f.state = FoxState::Prowl;
      audioPlayTone(250, 60);
    }
    return true;
  }
  for (auto& f : frogs) {
    if (!f.active || !hits(f.x, f.y, FROG_W, FROG_H)) continue;
    if (ball) {
      f.active = false;
      knockOff(sfbFrog, FROG_W, FROG_H, f.x, f.y, fromX);
    } else {
      f.stun = STUN_TICKS;
      f.knockVx = knock;
      f.vy = 0;
      f.wait = 0;
      f.dashVx = 0;
      audioPlayTone(250, 60);
    }
    return true;
  }
  return false;
}

void updateItems(const Player& p, Gorilla* gorillas, Status& st, uint32_t now,
                 int16_t screenH) {
  for (auto& it : items) {
    if (it.state != ItemState::Free) continue;
    const int16_t s = itemSize(it);

    // Sideways.
    if (it.vx != 0) {
      const ArrowDir way = it.vx > 0 ? ARROW_RIGHT : ARROW_LEFT;
      // Boxes that already overlap are let apart.
      const Item* other = cardboardAt(it.x + it.vx, it.y, s, &it);
      if (itemBlocked(it.x + it.vx, it.y, s, way, screenH) ||
          (other != nullptr && other != cardboardAt(it.x, it.y, s, &it))) {
        it.vx = -it.vx * ITEM_BOUNCE;
      } else {
        it.x += it.vx;
        if (it.kind == ItemKind::Ball) {
          // It turns as if rolling that far.
          const float spin = constrain(it.vx / (BALL_SIZE / 2.0f),
                                       -BALL_MAX_SPIN, BALL_MAX_SPIN);
          it.angle = fmodf(it.angle + spin, 2.0f * PI);
        }
      }
    }

    // Up and down.
    const float prevY = it.y;
    it.vy = min(it.vy + GRAVITY, MAX_FALL);
    if (it.vy < 0 && itemBlocked(it.x, it.y + it.vy, s, ARROW_UP, screenH)) {
      it.vy = 0;
    }
    it.y += it.vy;
    // Every direction block holds it up, whichever way its arrow points.
    const float surface = surfaceBelow(it.x, s, prevY, 4.0f, screenH);
    if (it.vy >= 0 && surface >= 0 && it.y >= surface) {
      it.y = surface;
      it.vy = 0;
      it.vx = it.vx > 0 ? max(0.0f, it.vx - ITEM_FRICTION)
                        : min(0.0f, it.vx + ITEM_FRICTION);
      if (it.vx == 0) it.thrown = false;
    }
    if (it.y > screenH + FALL_RESET_DEPTH) {
      it.state = ItemState::None;  // lost down a pit
      continue;
    }

    if (it.thrown) itemCollects(it, st, now, screenH);
    if (it.thrown && itemHitsEnemy(it, gorillas)) {
      it.thrown = false;
      it.vx = -it.vx * ITEM_BOUNCE;
    }

    const bool touching = p.x + PLAYER_W > it.x && p.x < it.x + s &&
                          p.y > it.y - s && p.y - boxHeight(p) < it.y;
    if (!touching) {
      it.armed = true;
      continue;
    }
    if (!it.armed) continue;
    if (p.ducking) {
      if (carriedItem() < 0 && carriedBean() < 0) {
        it.state = ItemState::Carried;
        it.thrown = false;
        audioPlayTone(784, 40);
      }
      continue;
    }
    // Walked into: it slides away ahead of the bird.
    const bool right = it.x + s / 2.0f >= p.x + PLAYER_W / 2.0f;
    if (right ? p.vx > 0.3f : p.vx < -0.3f) {
      const float speed = max(ITEM_SLIDE_SPEED, fabsf(p.vx) + 1.5f);
      it.vx = right ? speed : -speed;
      it.thrown = false;
    }
  }
}

void throwItem(Item& it, const Player& p) {
  const int16_t s = itemSize(it);
  it.state = ItemState::Free;
  it.x = constrain(p.x + (PLAYER_W - s) / 2.0f, 0.0f,
                   static_cast<float>(worldW() - s));
  it.y = p.y - (p.ducking ? 0 : 8);
  const float speed =
      it.kind == ItemKind::Ball ? BALL_THROW_SPEED : THROW_SPEED;
  it.vx = (p.facingRight ? speed : -speed) + p.vx * 0.3f;
  it.vy = THROW_LIFT;
  it.thrown = true;
  it.armed = false;
  audioPlayTone(600, 40);
}

// Set the carried item down in the cell just in front of the bird. False (it
// stays carried) if something is in the way.
bool placeItem(Item& it, const Player& p, int16_t screenH) {
  const int16_t col = p.facingRight ? columnAt(p.x + PLAYER_W - 0.01f) + 1
                                    : columnAt(p.x) - 1;
  const int16_t row =
      static_cast<int16_t>(floorf((screenH - (p.y - 1)) / TILE));
  if (col < 0 || col >= level->tiles || solidTile(col, row)) return false;
  const int16_t s = itemSize(it);
  float x = col * TILE + (TILE - s) / 2.0f;
  float bottom = rowBottom(row, screenH);
  // Cardboard boxes already there: it goes on top of them, up to a stack of
  // three.
  for (uint8_t tries = 0; tries < 2; ++tries) {
    const Item* under = cardboardAt(x, bottom, s, &it);
    if (under == nullptr) break;
    x = under->x + (CARDBOARD_SIZE - s) / 2.0f;
    bottom = under->y - CARDBOARD_SIZE;
  }
  if (cardboardAt(x, bottom, s, &it) != nullptr ||
      itemBlocked(x, bottom, s, ARROW_UP, screenH)) {
    return false;
  }
  it.state = ItemState::Free;
  it.x = x;
  it.y = bottom;
  it.vx = 0;
  it.vy = 0;
  it.thrown = false;
  it.armed = false;
  audioPlayTone(523, 40);
  return true;
}

// ---- Beans and vines --------------------------------------------------------
int carriedBean() {
  for (int i = 0; i < BEAN_SLOTS; ++i) {
    if (beans[i].state == BeanState::Carried) return i;
  }
  return -1;
}

// skyY is the world y of the highest the camera can see.
void updateBeans(const Player& p, int16_t screenH, float skyY) {
  for (auto& n : beans) {
    if (n.state == BeanState::Vine) {
      n.top = max(skyY, n.top - VINE_GROW_SPEED);
      continue;
    }
    if (n.state == BeanState::Emerging) {
      n.y -= EGG_EMERGE_SPEED;
      if (n.y <= n.restY) {
        n.y = n.restY;
        n.state = BeanState::Free;
      }
      continue;
    }
    if (n.state != BeanState::Free) continue;

    // Falls unless something is under it.
    const float prevY = n.y;
    n.vy = min(n.vy + GRAVITY, MAX_FALL);
    n.y += n.vy;
    // Direction blocks treat it like the bird: only an up one holds it.
    const float surface =
        surfaceBelow(n.col * TILE, TILE, prevY, 4.0f, screenH, true);
    if (surface < 0 || n.y < surface) {
      if (n.y > screenH + FALL_RESET_DEPTH) {
        // Down a pit: it drops out of the sky again where it fell.
        n.y = skyY;
        n.vy = 0;
      }
      continue;
    }
    n.y = surface;
    n.vy = 0;

    const float left = n.col * TILE;
    const bool touching = p.x + PLAYER_W > left + 2 &&
                          p.x < left + TILE - 2 && p.y > n.y - TILE &&
                          p.y - boxHeight(p) < n.y;
    if (!touching) {
      n.armed = true;
      continue;
    }
    if (!n.armed) continue;
    if (!p.ducking) {
      n.state = BeanState::Vine;
      n.top = n.y;
      audioPlayTone(1046, 120);
    } else if (carriedBean() < 0 && carriedItem() < 0) {
      n.state = BeanState::Carried;  // it rides on the bird's back
      audioPlayTone(784, 40);
    }
  }
}

// Put the carried bean in the cell just in front of the bird. False (it stays
// on the bird's back) if something is in the way.
bool placeBean(Bean& n, const Player& p, int16_t screenH) {
  const int16_t col = p.facingRight ? columnAt(p.x + PLAYER_W - 0.01f) + 1
                                    : columnAt(p.x) - 1;
  const int16_t row =
      static_cast<int16_t>(floorf((screenH - (p.y - 1)) / TILE));
  if (col < 0 || col >= level->tiles || solidTile(col, row)) return false;
  const float top = rowTop(row, screenH);
  for (const auto& b : boxes) {
    if (!b.broken && b.x < (col + 1) * TILE && b.x + b.w > col * TILE &&
        b.y < top + TILE && b.y + b.h > top) {
      return false;
    }
  }
  n.state = BeanState::Free;
  n.col = col;
  n.y = rowBottom(row, screenH);
  n.vy = 0;
  n.armed = true;
  audioPlayTone(523, 40);
  return true;
}

// A vine the bird is touching, or -1.
int findVine(const Player& p) {
  for (int i = 0; i < BEAN_SLOTS; ++i) {
    const Bean& n = beans[i];
    if (n.state != BeanState::Vine) continue;
    const float center = n.col * TILE + TILE / 2.0f;
    if (fabsf(p.x + PLAYER_W / 2.0f - center) <= 12.0f && p.y > n.top - 4 &&
        p.y - boxHeight(p) < n.y) {
      return i;
    }
  }
  return -1;
}

// Which sublevel (index into main.subs) a vine planted at (col, row) of the
// map `here` leads to, or -1 if it leads nowhere. An exact spot beats a column
// mark; the rest go to sublevel 0.
int sublevelFor(const Level& here, const Level& main, int16_t col,
                int16_t row) {
  int wanted = -1;
  for (const int16_t atRow : {row, static_cast<int16_t>(-1)}) {
    for (uint8_t i = 0; wanted < 0 && i < here.spotCount; ++i) {
      if (here.spots[i][0] == col && here.spots[i][1] == atRow) {
        wanted = here.spots[i][2];
      }
    }
  }
  for (const int number : {wanted, 0}) {
    for (uint8_t i = 0; i < main.subCount; ++i) {
      if (main.subNumbers[i] == number) return i;
    }
  }
  return -1;
}

// Everything in the main level that is kept while the bird is in a sublevel.
struct AreaState {
  Box boxes[BOX_SLOTS];
  Egg eggs[SPECIAL_SLOTS];
  Rat rats[SPECIAL_SLOTS];
  Bean beans[BEAN_SLOTS];
  Item items[ITEM_SLOTS];
  Fox foxes[FOX_MAX];
  Frog frogs[FROG_MAX];
  Gorilla gorillas[MAX_GORILLAS];
  bool goldTaken[LEVEL_MAX_GOLDS];
  uint8_t goldCount;
  bool infinityTaken[LEVEL_MAX_INFINITIES];
  uint8_t infinityCount;
  bool coinTaken[LEVEL_MAX_COINS];
  uint16_t coinCount;
  bool greenTaken[LEVEL_MAX_GREENS];
};

// Several KB, so it is only held while the game runs. Null if there was no
// room, and then the vines lead nowhere.
AreaState* mainArea = nullptr;

struct AreaRoom {
  AreaRoom() { mainArea = static_cast<AreaState*>(malloc(sizeof(AreaState))); }
  ~AreaRoom() {
    free(mainArea);
    mainArea = nullptr;
  }
};

void saveArea(const Gorilla* gorillas) {
  memcpy(mainArea->boxes, boxes, sizeof(boxes));
  memcpy(mainArea->eggs, eggs, sizeof(eggs));
  memcpy(mainArea->rats, rats, sizeof(rats));
  memcpy(mainArea->beans, beans, sizeof(beans));
  memcpy(mainArea->items, items, sizeof(items));
  memcpy(mainArea->foxes, foxes, sizeof(foxes));
  memcpy(mainArea->frogs, frogs, sizeof(frogs));
  memcpy(mainArea->gorillas, gorillas, sizeof(mainArea->gorillas));
  memcpy(mainArea->goldTaken, goldTaken, sizeof(goldTaken));
  mainArea->goldCount = goldCount;
  goldsFromMain = goldCount;
  memcpy(mainArea->infinityTaken, infinityTaken, sizeof(infinityTaken));
  mainArea->infinityCount = infinityCount;
  memcpy(mainArea->coinTaken, coinTaken, sizeof(coinTaken));
  mainArea->coinCount = coinCount;
  memcpy(mainArea->greenTaken, greenTaken, sizeof(greenTaken));
}

void restoreArea(Gorilla* gorillas) {
  memcpy(boxes, mainArea->boxes, sizeof(boxes));
  memcpy(eggs, mainArea->eggs, sizeof(eggs));
  memcpy(rats, mainArea->rats, sizeof(rats));
  memcpy(beans, mainArea->beans, sizeof(beans));
  memcpy(items, mainArea->items, sizeof(items));
  memcpy(foxes, mainArea->foxes, sizeof(foxes));
  memcpy(frogs, mainArea->frogs, sizeof(frogs));
  memcpy(gorillas, mainArea->gorillas, sizeof(mainArea->gorillas));
  memcpy(goldTaken, mainArea->goldTaken, sizeof(goldTaken));
  goldCount = mainArea->goldCount;
  memcpy(infinityTaken, mainArea->infinityTaken, sizeof(infinityTaken));
  infinityCount = mainArea->infinityCount;
  memcpy(coinTaken, mainArea->coinTaken, sizeof(coinTaken));
  coinCount = mainArea->coinCount;
  memcpy(greenTaken, mainArea->greenTaken, sizeof(greenTaken));
  clearParticles();
  for (auto& c : corpses) c.active = false;
  bumpedBox = nullptr;
}

// Take the carried bean out of the current area, if there is one.
bool takeCarriedBean(Bean& out) {
  const int i = carriedBean();
  if (i < 0) return false;
  out = beans[i];
  beans[i].state = BeanState::None;
  return true;
}

// Bring a carried bean into the current area.
void giveCarriedBean(const Bean& bean) {
  int slot = SPECIAL_SLOTS;
  while (slot < BEAN_SLOTS - 1 && beans[slot].state != BeanState::None) ++slot;
  beans[slot] = bean;
}

// The same for a carried box or ball.
bool takeCarriedItem(Item& out) {
  const int i = carriedItem();
  if (i < 0) return false;
  out = items[i];
  items[i].state = ItemState::None;
  return true;
}

void giveCarriedItem(const Item& item) {
  int slot = 0;
  while (slot < ITEM_SLOTS - 1 && items[slot].state != ItemState::None) ++slot;
  items[slot] = item;
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

void copySuperFlashySky(uint8_t* frame, int16_t offset) {
  copySky(frame, offset, sfbSkyHeight);
}

void drawSuperFlashySky(TFT_eSPI& d, int16_t offset, int16_t width,
                        int16_t height) {
  if (sfbSkyRuns == nullptr || sfbSkyRows == nullptr || width <= 0 ||
      height <= 0) {
    d.fillRect(0, 0, width, height, 0x5DFF);
    return;
  }
  if (offset < 0) offset = 0;
  offset %= sfbSkyWidth;
  uint8_t line[sfbSkyWidth];
  const int16_t rows = height < sfbSkyHeight ? height : sfbSkyHeight;
  for (int16_t y = 0; y < rows; ++y) {
    skyRow(y, line);
    int16_t x = 0;
    while (x < width) {
      const uint8_t color = line[(x + offset) % sfbSkyWidth];
      int16_t end = x + 1;
      while (end < width && line[(end + offset) % sfbSkyWidth] == color) {
        ++end;
      }
      d.drawFastHLine(x, y, end - x, d.color8to16(color));
      x = end;
    }
  }
}

void drawSuperFlashySkyThumbnail(TFT_eSPI& d, int16_t x, int16_t y, int16_t w,
                                 int16_t h) {
  if (sfbSkyRuns == nullptr || sfbSkyRows == nullptr) {
    d.fillRect(x, y, w, h, 0x5DFF);
    return;
  }
  uint8_t line[sfbSkyWidth];
  for (int16_t dy = 0; dy < h; ++dy) {
    skyRow(dy * sfbSkyHeight / h, line);
    for (int16_t dx = 0; dx < w; ++dx) {
      d.drawPixel(x + dx, y + dy, d.color8to16(line[dx * sfbSkyWidth / w]));
    }
  }
}

void drawSuperFlashyBirdsIcon(TFT_eSPI& d, int16_t x, int16_t y) {
  drawSuperFlashySkyThumbnail(d, x, y, 80, 58);
  if (sfbGrass == nullptr || sfbBird == nullptr) return;
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
  // A mystery box over a brick on the right edge, and a golden egg on the
  // ground at the left.
  const auto stamp = [&](const uint16_t* art, int16_t left, int16_t top) {
    for (int16_t ty = 0; ty < 16; ++ty) {
      for (int16_t tx = 0; tx < 16; ++tx) {
        const uint16_t c = pgm_read_word(&art[ty * 16 + tx]);
        if (c != SFB_TRANSPARENT) d.drawPixel(left + tx, top + ty, c);
      }
    }
  };
  stamp(sfbMystery, x + 64, y + 6);
  stamp(sfbBox, x + 64, y + 22);
  stamp(sfbGoldEgg, x, y + 26);
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
  // Take the frame buffer before the sprites, so it is one contiguous block.
  // Loading the art first splits the heap and this allocation fails, which
  // makes every tile paint straight to the panel.
  TFT_eSprite frame(&display);
  frame.setColorDepth(8);
  frame.createSprite(screenW, screenH);
  const AreaRoom areaRoom;

  SuperFlashyAssets art;
  if (!loadSuperFlashyAssets(art)) {
    frame.deleteSprite();
    if (!loadSuperFlashyAssets(art)) {
      display.fillScreen(TFT_BLACK);
      display.setTextDatum(MC_DATUM);
      display.setTextColor(TFT_WHITE, TFT_BLACK);
      display.drawString("Game art unavailable", display.width() / 2,
                         display.height() / 2);
      delay(1200);
      return;
    }
  }
  if (!frame.created()) {
    frame.setColorDepth(8);
    frame.createSprite(screenW, screenH);
  }
  bool fullFrame = frame.created();
  int16_t stripRows = screenH;
  if (!fullFrame) {
    // Wider strips mean fewer passes. 120 rows is half a screen.
    const int16_t choices[] = {120, 80, 48};
    for (int16_t rows : choices) {
      frame.setColorDepth(8);
      frame.createSprite(screenW, rows);
      if (frame.created()) {
        stripRows = rows;
        break;
      }
    }
  }
  bindSuperFlashyArt(art);

  waitForButtonRelease();
  audioBegin();

  gFrameBuffer =
      fullFrame ? static_cast<uint8_t*>(frame.getPointer()) : nullptr;

  level = &LEVELS[0];
  int16_t worldH = level->rows * TILE;  // physics "screenH": the world bottom
  float cameraYf = 0;
  bool snapCamera = true;

  Player p;
  respawn(p, worldH);
  Gorilla gorillas[MAX_GORILLAS];
  uint8_t levelIndex = 0;
  initGorillas(gorillas, worldH);
  initObjects(worldH);
  Status status = {PLAYER_MAX_HP, 0, 0, START_LIVES};
  float visualSink = BIRD_SINK;

  // Double-press detection: when a direction button is pressed again within
  // DOUBLE_PRESS_MS of its last press, running turns on until it is released.
  Button lastDirection = Button::None;
  uint32_t lastDirectionPress = 0;
  Button steadyDirection = Button::None;  // held without a break
  uint32_t steadySince = 0;
  uint32_t exitHeldSince = 0;
  uint32_t lastDirectionHeld = 0;
  uint32_t jumpBufferUntil = 0;
  uint32_t stompBoostUntil = 0;  // a jump press until then boosts off a stomp
  bool stompBoosting = false;    // rising from a boost: letting go keeps it
  bool crouchWanted = false;  // BTN2 toggles it
  uint32_t levelMs = 0;  // time on the current level (running through deaths)
  uint32_t totalMs = 0;  // time across all levels
  uint32_t lastTick = millis();
  bool gliding = false;       // toggled by jump in the air; ends on landing
  uint32_t lastFrame = millis();
  Button pendingDirection = Button::None;
  bool pendingJump = false;
  bool pendingCrouch = false;
  int climbing = -1;          // index of the vine being climbed
  bool inSublevel = false;
  int16_t vineCol = 0;        // column of the vine that led to the sublevel
  bool exitNeedsRelease = false;  // BTN3 was used for climbing
  uint32_t crouchHoldStart = 0;   // crouch pressed while carrying something
  bool dropCued = false;          // beeped: letting go now sets the item down

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
    if (held != Button::Button3) exitNeedsRelease = false;
    if (climbing >= 0 && held == Button::Button3) exitNeedsRelease = true;
    if (held == Button::Button3 && !exitNeedsRelease) {
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
    // The world y of the highest the camera can see.
    const float skyY = min<float>(0, worldH - screenH);

    // A direction press steps off a vine.
    if (climbing >= 0 &&
        (event == Button::Button1 || event == Button::Button4)) {
      climbing = -1;
      p.vx = event == Button::Button1 ? -WALK_SPEED : WALK_SPEED;
    }

    // A jump needs a fresh press (holding BTN3 to exit must not bounce).
    if (pendingJump) {
      pendingJump = false;
      const int vine = climbing < 0 ? findVine(p) : -1;
      if (vine >= 0) {  // grab the vine
        climbing = vine;
        exitNeedsRelease = true;
        exitHeldSince = 0;
        jumpBufferUntil = 0;
        gliding = false;
        crouchWanted = false;
        p.running = false;
      } else if (climbing < 0 && now < stompBoostUntil) {
        p.vy = STOMP_BOOST_VELOCITY;
        p.onGround = false;
        stompBoostUntil = 0;
        stompBoosting = true;
        jumpBufferUntil = 0;
        gliding = false;
        audioPlayTone(1100, 60);
      } else if (climbing < 0) {
        jumpBufferUntil = now + JUMP_BUFFER_MS;
        // Pressing jump again while already in the air toggles the glide.
        if (!p.onGround) gliding = !gliding;
      }
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
    const Button direction = wantLeft || wantRight ? held : Button::None;
    if (direction != steadyDirection) {
      steadyDirection = direction;
      steadySince = now;
    } else if (direction != Button::None && p.onGround && !p.ducking &&
               now - steadySince >= RUN_HOLD_MS) {
      p.running = true;
    }

    // BTN2 toggles the crouch (one block tall). If a block is overhead the bird
    // is held crouched even after toggling off, and stands up by itself as soon
    // as it is out from under it.
    // Crouch toggles as usual. Carrying a bean, the toggle waits for the button
    // to be let go: held for a second, it sets the bean down instead.
    if (pendingCrouch) {
      pendingCrouch = false;
      if (climbing >= 0) {
        // Held, it climbs down.
      } else if (carriedBean() >= 0 || carriedItem() >= 0) {
        crouchHoldStart = now;
        dropCued = false;
      } else {
        crouchWanted = !crouchWanted;
      }
    }
    if (crouchHoldStart != 0) {
      const int carried = carriedBean();
      const int item = carriedItem();
      if (climbing >= 0 || (carried < 0 && item < 0)) {
        crouchHoldStart = 0;
      } else if (item >= 0) {
        // Decided on release: a tap toggles the crouch, a hold throws, a long
        // hold sets the item down.
        const uint32_t heldMs = now - crouchHoldStart;
        if (held == Button::Button2) {
          if (heldMs >= ITEM_DROP_HOLD_MS && !dropCued) {
            dropCued = true;
            audioPlayTone(440, 30);
          }
        } else {
          crouchHoldStart = 0;
          if (heldMs >= ITEM_DROP_HOLD_MS) {
            if (placeItem(items[item], p, worldH)) {
              crouchWanted = true;
              p.vx = 0;
              lastDirectionHeld = 0;
            }
          } else if (heldMs >= THROW_HOLD_MS) {
            throwItem(items[item], p);
          } else {
            crouchWanted = !crouchWanted;
          }
        }
      } else if (held != Button::Button2) {
        crouchWanted = !crouchWanted;  // a short press
        crouchHoldStart = 0;
      } else if (now - crouchHoldStart >= BEAN_DROP_HOLD_MS) {
        crouchHoldStart = 0;
        if (placeBean(beans[carried], p, worldH)) {
          crouchWanted = true;  // set down from a crouch
          p.vx = 0;
          lastDirectionHeld = 0;
        }
      }
    }
    if (climbing >= 0) {
      // On a vine: hold jump to go up, crouch to go down, else hang. Blocks
      // do not stop a climbing bird.
      const Bean& vine = beans[climbing];
      p.x = vine.col * TILE + (TILE - PLAYER_W) / 2.0f;
      p.vx = 0;
      p.vy = 0;
      p.onGround = false;
      p.ducking = false;
      stompBoosting = false;
      if (wantJump) p.y -= CLIMB_SPEED;
      if (held == Button::Button2) p.y += CLIMB_SPEED;
      if (p.y < vine.top) p.y = vine.top;
      if (held == Button::Button2 && p.y >= vine.y) {  // climbed down
        p.y = vine.y;
        p.onGround = true;
        climbing = -1;
      }
      gBirdPrevY = p.y;  // climbing never stomps
    } else {
      gBirdPrevY = p.y;
      p.ducking = crouchWanted || !hasHeadroom(p, worldH);
      if (p.ducking) p.running = false;

      // Horizontal movement (a crouched bird has its own speed).
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
        p.vy = JUMP_VELOCITY;
        p.onGround = false;
        audioPlayTone(660, 25);
      }
      if (p.vy >= 0) stompBoosting = false;
      if (!wantJump && !stompBoosting && p.vy < JUMP_CUT_VELOCITY) {
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
      breakBricksAhead(p, prevX, gorillas);
      resolveWalls(p, prevX, worldH);
      bumpHead(p, worldH);
      unstick(p, worldH);
    }
    if (p.onGround) gliding = false;
    const bool fell = p.y > worldH + FALL_RESET_DEPTH;
    if (fell && !inSublevel) audioPlayTone(220, 120);

    if (bumpedBox != nullptr) {
      knockEnemiesOn(*bumpedBox, gorillas, p.x + PLAYER_W / 2.0f);
      bumpedBox = nullptr;
    }
    updateItems(p, gorillas, status, now, worldH);
    gStomped = false;
    updateGorillas(gorillas, p, worldH);
    updateParticles();
    updateCorpses(worldH);
    bool levelDone = updateObjects(p, status, now, worldH);
    updateBeans(p, worldH, skyY);
    if (gStomped) {
      if (now < jumpBufferUntil) {  // pressed just before landing
        p.vy = STOMP_BOOST_VELOCITY;
        stompBoosting = true;
        stompBoostUntil = 0;
        jumpBufferUntil = 0;
        gliding = false;
        audioPlayTone(1100, 60);
      } else {
        stompBoosting = false;
        stompBoostUntil = now + STOMP_BOOST_MS;
      }
    }
    // Being bitten or bumped knocks the bird off a vine.
    if (climbing >= 0 && (p.vx != 0 || p.vy != 0)) climbing = -1;

    bool gameOver = false;
    if (fell && inSublevel && status.hp > 0) {
      // Falling out of a sublevel is safe: drop back in from the sky above the
      // vine that led there.
      Bean carriedAlong;
      const bool bringBean = takeCarriedBean(carriedAlong);
      Item itemAlong;
      const bool bringItem = takeCarriedItem(itemAlong);
      inSublevel = false;
      level = &LEVELS[levelIndex];
      worldH = level->rows * TILE;
      restoreArea(gorillas);
      if (bringBean) giveCarriedBean(carriedAlong);
      if (bringItem) giveCarriedItem(itemAlong);
      p = {static_cast<float>(vineCol * TILE),
           min<float>(0, worldH - screenH),
           0, 0, false, p.facingRight, false, false, 0};
      snapCamera = true;
      crouchWanted = false;
      gliding = false;
      climbing = -1;
    } else if (fell || status.hp <= 0) {
      levelDone = false;  // dying on the nest does not count
      gameOver = --status.lives <= 0;
      climbing = -1;
      if (!gameOver) {  // try this level again, from the main map
        inSublevel = false;
        level = &LEVELS[levelIndex];
        worldH = level->rows * TILE;
        respawn(p, worldH);
        initGorillas(gorillas, worldH);
        initObjects(worldH);
        snapCamera = true;
        crouchWanted = false;
        gliding = false;
        status = {PLAYER_MAX_HP, 0, 0, status.lives};
      }
    } else if (climbing >= 0 && p.y <= skyY + 0.5f) {
      // Climbed off the top of the screen.
      const Bean& vine = beans[climbing];
      const int16_t row =
          static_cast<int16_t>(lroundf((worldH - vine.y) / TILE));
      const Level& main = LEVELS[levelIndex];
      const int target = sublevelFor(*level, main, vine.col, row);
      if (target >= 0 && mainArea != nullptr) {
        Bean carriedAlong;
        const bool bringBean = takeCarriedBean(carriedAlong);
        Item itemAlong;
        const bool bringItem = takeCarriedItem(itemAlong);
        if (!inSublevel) {  // falling out later returns above this vine
          vineCol = vine.col;
          saveArea(gorillas);
        }
        inSublevel = true;
        level = &main.subs[target];
        worldH = level->rows * TILE;
        respawn(p, worldH);
        if (level->startRow < 0) {  // no S in the sublevel: drop in
          p.y = min<float>(0, worldH - screenH);
          p.onGround = false;
        }
        initGorillas(gorillas, worldH);
        initObjects(worldH);
        if (bringBean) giveCarriedBean(carriedAlong);
        if (bringItem) giveCarriedItem(itemAlong);
        snapCamera = true;
        crouchWanted = false;
        gliding = false;
        climbing = -1;
        jumpBufferUntil = 0;
        audioPlayTone(1568, 150);
      }
    }
    // Ease the sink so the bird glides on and off boxes instead of popping.
    const float sinkTarget = targetSink(p, worldH);
    visualSink += constrain(sinkTarget - visualSink, -3.0f, 3.0f);
    const Hud hud = {
        exitHeld,
        now < status.speedBoostUntil ? status.speedBoostUntil - now : 0,
        status.hp, status.lives,
        now < status.invulnUntil && (now / 100) % 2 == 0, gliding,
        climbing >= 0, visualSink,
        levelMs, totalMs,
        crouchHoldStart != 0 && dropCued};

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

    const auto drawScene = [&](TFT_eSPI& target) {
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
        snprintf(times, sizeof(times), "Level %s   Total %s", levelTime,
                 totalTime);
        const bool last = levelIndex + 1 == LEVEL_COUNT;
        drawBanner(target, title, times,
                   last ? "All done! Back to level 1" : "Loading next level");
      } else if (gameOver) {
        drawBanner(target, "Game over", "Out of lives", "Back to level 1");
      }
    };
    if (fullFrame) {
      gClipTop = 0;
      gClipBottom = screenH;
      drawScene(frame);
      frame.pushSprite(0, 0);
    } else if (frame.created()) {
      // Each strip draws only its own rows, so the frame costs one pass.
      for (int16_t top = 0; top < screenH; top += stripRows) {
        gClipTop = top;
        gClipBottom = top + stripRows;
        frame.resetViewport();
        frame.fillSprite(TFT_BLACK);
        frame.setViewport(0, -top, screenW, screenH, true);
        drawScene(frame);
        frame.resetViewport();
        frame.pushSprite(0, top);
      }
      gClipTop = 0;
      gClipBottom = screenH;
    } else {
      gClipTop = 0;
      gClipBottom = screenH;
      drawScene(display);
    }

    if (levelDone || gameOver) {
      audioPlayTone(gameOver ? 160 : 1320, gameOver ? 300 : 80);
      delay(1800);
      lastTick = millis();
      levelMs = 0;
      levelIndex = gameOver ? 0 : (levelIndex + 1) % LEVEL_COUNT;
      if (levelIndex == 0) totalMs = 0;  // a new run
      if (gameOver) {
        status.hp = PLAYER_MAX_HP;
        status.lives = START_LIVES;
      }
      level = &LEVELS[levelIndex];
      worldH = level->rows * TILE;
      inSublevel = false;
      climbing = -1;
      snapCamera = true;
      crouchWanted = false;
      gliding = false;
      respawn(p, worldH);
      initGorillas(gorillas, worldH);
      initObjects(worldH);
      status.invulnUntil = 0;
      status.speedBoostUntil = 0;
      visualSink = BIRD_SINK;
      jumpBufferUntil = 0;
      lastFrame = millis();
    }
  }

  audioEnd();
  frame.deleteSprite();
  gFrameBuffer = nullptr;
  art.release();
  waitForButtonRelease();
}

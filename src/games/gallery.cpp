// Gallery: a tiny drawing program driven by the four buttons.
//
// Draw mode   BTN1 left, BTN2 up, BTN3 down, BTN4 right (hold to repeat).
//             Double-tap BTN2 (up): pen down / pen up.
//             Double-tap BTN1: open / close tool mode.
// Tool mode   BTN1 next tool, BTN2 next size/option, BTN3 colour map.
//             Double-tap BTN1: close.  Hold BTN4 for 3 seconds: exit game.
// Colour mode BTN1/2/3/4 move left/up/down/right through the colour map.
//             Double-tap BTN1: pick the colour.  Double-tap BTN2: cancel.
//
// Shape tools (Line, Rect, Oval): pen down sets the first corner, move to
// stretch the shape, pen up stamps it. Fill: pen down floods the area.
//
// The picture lives in a 320x240 RGB332 buffer (one byte per pixel), so the
// colour map is exactly the set of colours the canvas can hold.

#include "games/gallery.h"

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "input/button_input.h"

namespace {
constexpr int16_t W = 320;
constexpr int16_t H = 240;
constexpr uint8_t WHITE332 = 0xFF;
constexpr uint8_t BLACK332 = 0x00;

constexpr uint32_t DOUBLE_TAP_MS = 350;   // max gap between press starts
constexpr uint32_t TAP_MAX_MS = 250;      // a tap is released this fast
constexpr uint32_t REPEAT_DELAY_MS = 280;
constexpr uint32_t DRAW_REPEAT_MS = 16;
constexpr uint32_t COLOR_REPEAT_MS = 110;
constexpr uint32_t EXIT_HOLD_MS = 3000;
constexpr uint32_t HINT_MS = 3500;

constexpr int16_t PANEL_H = 50;
constexpr uint16_t PANEL_BG = 0x2104;
constexpr uint16_t PANEL_CHIP = 0x4A69;

constexpr int16_t MAP_X = 16;
constexpr int16_t MAP_Y = 24;
constexpr int16_t MAP_CELL = 12;
constexpr int16_t MAP_N = 16;

enum class Mode : uint8_t { Draw, Tools, Color };
enum class Tool : uint8_t { Pen, Spray, Eraser, Fill, Line, Rect, Oval };
constexpr uint8_t TOOL_COUNT = 7;

constexpr const char* TOOL_NAMES[TOOL_COUNT] = {"Pen",  "Spray", "Erase", "Fill",
                                                "Line", "Rect",  "Oval"};
constexpr uint8_t PEN_SIZES[] = {1, 2, 4, 6, 10, 16};
constexpr uint8_t SPRAY_SIZES[] = {10, 18, 28};
constexpr uint8_t ERASER_SIZES[] = {4, 8, 14, 22, 34};
constexpr uint8_t LINE_WIDTHS[] = {1, 2, 4, 8};
constexpr uint8_t SHAPE_THICK[] = {1, 3, 6, 0};  // 0 = filled
constexpr const char* SHAPE_LABELS[] = {"Outline thin", "Outline medium",
                                        "Outline thick", "Filled"};

struct State {
  int16_t px, py;      // pen position
  Tool tool;
  uint8_t option[TOOL_COUNT];
  int8_t cellX, cellY;  // colour-map cursor
};

// Everything below is reset by runGallery().
TFT_eSPI* tft = nullptr;
uint8_t* canvas = nullptr;
uint16_t lut[256];
uint8_t palette[MAP_N * MAP_N];

State st;
Mode mode = Mode::Draw;
uint8_t color = BLACK332;
bool penDown = false;
int16_t anchorX = 0, anchorY = 0;

int16_t dMinX, dMinY, dMaxX, dMaxY;  // dirty rectangle (canvas -> screen)

struct Box {
  int16_t x0, y0, x1, y1;
  bool valid;
};
Box oldCursor = {0, 0, 0, 0, false};
Box oldPreview = {0, 0, 0, 0, false};

// ---------------------------------------------------------------- helpers

uint8_t quantize(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((r & 0xE0) | ((g & 0xE0) >> 3) | (b >> 6));
}

void buildLut() {
  for (int i = 0; i < 256; ++i) {
    const uint8_t r = ((i >> 5) & 7) * 255 / 7;
    const uint8_t g = ((i >> 2) & 7) * 255 / 7;
    const uint8_t b = (i & 3) * 255 / 3;
    lut[i] = tft->color565(r, g, b);
  }
}

void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  const float c = v * s;
  const float hp = h / 60.0f;
  const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float rf = 0, gf = 0, bf = 0;
  if (hp < 1) { rf = c; gf = x; }
  else if (hp < 2) { rf = x; gf = c; }
  else if (hp < 3) { gf = c; bf = x; }
  else if (hp < 4) { gf = x; bf = c; }
  else if (hp < 5) { rf = x; bf = c; }
  else { rf = c; bf = x; }
  const float m = v - c;
  r = static_cast<uint8_t>((rf + m) * 255.0f + 0.5f);
  g = static_cast<uint8_t>((gf + m) * 255.0f + 0.5f);
  b = static_cast<uint8_t>((bf + m) * 255.0f + 0.5f);
}

// Row 0: greys (white -> black). Rows 1..15: 16 hues, light/pastel -> pure ->
// dark.
void buildPalette() {
  for (int row = 0; row < MAP_N; ++row) {
    for (int col = 0; col < MAP_N; ++col) {
      uint8_t r, g, b;
      if (row == 0) {
        const uint8_t v = static_cast<uint8_t>(255 - col * 17);
        r = g = b = v;
      } else {
        const int t = row - 1;  // 0..14
        float s = 1.0f, v = 1.0f;
        if (t < 7) {
          s = 0.25f + 0.75f * t / 6.0f;
        } else {
          v = 1.0f - (t - 6) * 0.09375f;
        }
        hsvToRgb(col * 360.0f / MAP_N, s, v, r, g, b);
      }
      palette[row * MAP_N + col] = quantize(r, g, b);
    }
  }
}

void markDirty(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  if (x0 < dMinX) dMinX = x0;
  if (y0 < dMinY) dMinY = y0;
  if (x1 > dMaxX) dMaxX = x1;
  if (y1 > dMaxY) dMaxY = y1;
}

void resetDirty() {
  dMinX = W;
  dMinY = H;
  dMaxX = -1;
  dMaxY = -1;
}

// Copy a canvas rectangle to the screen.
void blit(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;
  if (x1 < x0 || y1 < y0) return;
  uint16_t line[W];
  const int16_t w = x1 - x0 + 1;
  for (int16_t y = y0; y <= y1; ++y) {
    const uint8_t* src = canvas + y * W + x0;
    for (int16_t i = 0; i < w; ++i) line[i] = lut[src[i]];
    tft->pushImage(x0, y, w, 1, line);
  }
}

void flushDirty() {
  if (dMaxX >= dMinX && dMaxY >= dMinY) blit(dMinX, dMinY, dMaxX, dMaxY);
  resetDirty();
}

inline void put(int16_t x, int16_t y, uint8_t c) {
  if (static_cast<uint16_t>(x) >= W || static_cast<uint16_t>(y) >= H) return;
  canvas[y * W + x] = c;
  if (x < dMinX) dMinX = x;
  if (x > dMaxX) dMaxX = x;
  if (y < dMinY) dMinY = y;
  if (y > dMaxY) dMaxY = y;
}

void hSpan(int16_t x0, int16_t x1, int16_t y, uint8_t c) {
  if (y < 0 || y >= H) return;
  if (x0 < 0) x0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (x1 < x0) return;
  memset(canvas + y * W + x0, c, x1 - x0 + 1);
  markDirty(x0, y, x1, y);
}

void fillRectCanvas(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    uint8_t c) {
  for (int16_t y = y0; y <= y1; ++y) hSpan(x0, x1, y, c);
}

void stampDisc(int16_t cx, int16_t cy, int d, uint8_t c) {
  if (d <= 1) {
    put(cx, cy, c);
    return;
  }
  const int half = d / 2;
  const int even = (d % 2 == 0) ? 1 : 0;
  for (int yy = cy - half; yy < cy - half + d; ++yy) {
    const int dy2 = 2 * (yy - cy) + even;
    for (int xx = cx - half; xx < cx - half + d; ++xx) {
      const int dx2 = 2 * (xx - cx) + even;
      if (dx2 * dx2 + dy2 * dy2 <= d * d) put(xx, yy, c);
    }
  }
}

void stampSquare(int16_t cx, int16_t cy, int d, uint8_t c) {
  const int half = d / 2;
  fillRectCanvas(cx - half, cy - half, cx - half + d - 1, cy - half + d - 1,
                 c);
}

void spray(int16_t cx, int16_t cy, int d, uint8_t c) {
  const int r = d / 2;
  const int dots = d / 3 + 2;
  for (int i = 0; i < dots; ++i) {
    const int dx = static_cast<int>(random(-r, r + 1));
    const int dy = static_cast<int>(random(-r, r + 1));
    if (dx * dx + dy * dy <= r * r) put(cx + dx, cy + dy, c);
  }
}

template <typename F>
void walkLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, F visit) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (true) {
    visit(x0, y0);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// ----------------------------------------------------------------- tools

uint8_t optionCount(Tool t) {
  switch (t) {
    case Tool::Pen: return sizeof(PEN_SIZES);
    case Tool::Spray: return sizeof(SPRAY_SIZES);
    case Tool::Eraser: return sizeof(ERASER_SIZES);
    case Tool::Fill: return 1;
    case Tool::Line: return sizeof(LINE_WIDTHS);
    default: return 4;
  }
}

bool isShape(Tool t) {
  return t == Tool::Line || t == Tool::Rect || t == Tool::Oval;
}

void optionLabel(char* buf, size_t n, Tool t, uint8_t opt) {
  switch (t) {
    case Tool::Pen: snprintf(buf, n, "Size %d", PEN_SIZES[opt]); break;
    case Tool::Spray: snprintf(buf, n, "Size %d", SPRAY_SIZES[opt]); break;
    case Tool::Eraser: snprintf(buf, n, "Size %d", ERASER_SIZES[opt]); break;
    case Tool::Fill: snprintf(buf, n, "Pen down fills"); break;
    case Tool::Line: snprintf(buf, n, "Width %d", LINE_WIDTHS[opt]); break;
    default: snprintf(buf, n, "%s", SHAPE_LABELS[opt]); break;
  }
}

void stampAt(int16_t x, int16_t y) {
  const uint8_t o = st.option[static_cast<uint8_t>(st.tool)];
  switch (st.tool) {
    case Tool::Pen: stampDisc(x, y, PEN_SIZES[o], color); break;
    case Tool::Spray: spray(x, y, SPRAY_SIZES[o], color); break;
    case Tool::Eraser: stampSquare(x, y, ERASER_SIZES[o], WHITE332); break;
    default: break;
  }
}

void strokeTo(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  walkLine(x0, y0, x1, y1, [](int16_t x, int16_t y) { stampAt(x, y); });
}

void commitOval(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int t) {
  const float a = (x1 - x0 + 1) / 2.0f, b = (y1 - y0 + 1) / 2.0f;
  const float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
  for (int16_t y = y0; y <= y1; ++y) {
    const float dy = (y - cy) / b;
    const float half = a * sqrtf(fmaxf(0.0f, 1.0f - dy * dy));
    int16_t L = static_cast<int16_t>(ceilf(cx - half));
    int16_t R = static_cast<int16_t>(floorf(cx + half));
    if (L > R) L = R = static_cast<int16_t>(lroundf(cx));
    bool full = (t == 0);
    int16_t iL = 0, iR = -1;
    if (!full) {
      const float a2 = a - t, b2 = b - t;
      if (a2 <= 0 || b2 <= 0) {
        full = true;
      } else {
        const float dy2 = (y - cy) / b2;
        if (fabsf(dy2) >= 1.0f) {
          full = true;
        } else {
          const float h2 = a2 * sqrtf(1.0f - dy2 * dy2);
          iL = static_cast<int16_t>(ceilf(cx - h2));
          iR = static_cast<int16_t>(floorf(cx + h2));
          if (iL > iR) full = true;
        }
      }
    }
    if (full) {
      hSpan(L, R, y, color);
    } else {
      hSpan(L, max<int16_t>(L, iL - 1), y, color);
      hSpan(min<int16_t>(R, iR + 1), R, y, color);
    }
  }
}

void commitShape() {
  const uint8_t o = st.option[static_cast<uint8_t>(st.tool)];
  const int16_t x0 = min(anchorX, st.px), x1 = max(anchorX, st.px);
  const int16_t y0 = min(anchorY, st.py), y1 = max(anchorY, st.py);
  if (st.tool == Tool::Line) {
    const int w = LINE_WIDTHS[o];
    walkLine(anchorX, anchorY, st.px, st.py,
             [w](int16_t x, int16_t y) { stampDisc(x, y, w, color); });
  } else if (st.tool == Tool::Rect) {
    const int t = SHAPE_THICK[o];
    if (t == 0) {
      fillRectCanvas(x0, y0, x1, y1, color);
    } else {
      fillRectCanvas(x0, y0, x1, min<int16_t>(y0 + t - 1, y1), color);
      fillRectCanvas(x0, max<int16_t>(y1 - t + 1, y0), x1, y1, color);
      fillRectCanvas(x0, y0, min<int16_t>(x0 + t - 1, x1), y1, color);
      fillRectCanvas(max<int16_t>(x1 - t + 1, x0), y0, x1, y1, color);
    }
  } else if (st.tool == Tool::Oval) {
    commitOval(x0, y0, x1, y1, SHAPE_THICK[o]);
  }
}

void floodFill(int16_t sx, int16_t sy) {
  const uint8_t target = canvas[sy * W + sx];
  if (target == color) return;
  struct Seed { int16_t x, y; };
  constexpr int CAP = 1024;
  Seed* stack = static_cast<Seed*>(malloc(CAP * sizeof(Seed)));
  if (!stack) return;
  int sp = 0;
  stack[sp++] = {sx, sy};
  while (sp > 0) {
    const Seed s = stack[--sp];
    uint8_t* row = canvas + s.y * W;
    if (row[s.x] != target) continue;
    int16_t l = s.x, r = s.x;
    while (l > 0 && row[l - 1] == target) --l;
    while (r < W - 1 && row[r + 1] == target) ++r;
    hSpan(l, r, s.y, color);
    for (int dy = -1; dy <= 1; dy += 2) {
      const int16_t ny = s.y + dy;
      if (ny < 0 || ny >= H) continue;
      const uint8_t* nrow = canvas + ny * W;
      bool inRun = false;
      for (int16_t i = l; i <= r; ++i) {
        const bool m = nrow[i] == target;
        if (m && !inRun && sp < CAP) stack[sp++] = {i, ny};
        inRun = m;
      }
    }
  }
  free(stack);
}

// -------------------------------------------------------------- overlays

void drawCursor() {
  const int16_t x = st.px, y = st.py;
  TFT_eSPI& d = *tft;
  d.fillRect(x - 6, y - 1, 5, 3, TFT_WHITE);
  d.fillRect(x + 2, y - 1, 5, 3, TFT_WHITE);
  d.fillRect(x - 1, y - 6, 3, 5, TFT_WHITE);
  d.fillRect(x - 1, y + 2, 3, 5, TFT_WHITE);
  d.drawFastHLine(x - 5, y, 4, TFT_BLACK);
  d.drawFastHLine(x + 2, y, 4, TFT_BLACK);
  d.drawFastVLine(x, y - 5, 4, TFT_BLACK);
  d.drawFastVLine(x, y + 2, 4, TFT_BLACK);
  if (penDown) {
    d.fillRect(x - 2, y - 2, 5, 5, TFT_BLACK);
    d.fillRect(x - 1, y - 1, 3, 3, lut[color]);
  }
  oldCursor = {static_cast<int16_t>(x - 6), static_cast<int16_t>(y - 6),
               static_cast<int16_t>(x + 6), static_cast<int16_t>(y + 6), true};
}

void drawPreview() {
  if (!(penDown && isShape(st.tool))) return;
  TFT_eSPI& d = *tft;
  const uint8_t o = st.option[static_cast<uint8_t>(st.tool)];
  const int16_t x0 = min(anchorX, st.px), x1 = max(anchorX, st.px);
  const int16_t y0 = min(anchorY, st.py), y1 = max(anchorY, st.py);
  const uint16_t c = lut[color];
  if (st.tool == Tool::Line) {
    d.drawLine(anchorX, anchorY, st.px, st.py, c);
  } else if (st.tool == Tool::Rect) {
    if (SHAPE_THICK[o] == 0) d.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c);
    else d.drawRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, c);
  } else {
    const int16_t rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    const int16_t cx = x0 + rx, cy = y0 + ry;
    if (rx < 1 || ry < 1) d.drawLine(x0, y0, x1, y1, c);
    else if (SHAPE_THICK[o] == 0) d.fillEllipse(cx, cy, rx, ry, c);
    else d.drawEllipse(cx, cy, rx, ry, c);
  }
  oldPreview = {static_cast<int16_t>(x0 - 1), static_cast<int16_t>(y0 - 1),
                static_cast<int16_t>(x1 + 1), static_cast<int16_t>(y1 + 1),
                true};
}

// Erase the cursor/preview drawn last time, then draw them again.
void updateOverlays() {
  if (oldPreview.valid) {
    blit(oldPreview.x0, oldPreview.y0, oldPreview.x1, oldPreview.y1);
    oldPreview.valid = false;
  }
  if (oldCursor.valid) {
    blit(oldCursor.x0, oldCursor.y0, oldCursor.x1, oldCursor.y1);
    oldCursor.valid = false;
  }
  if (mode != Mode::Draw) return;
  drawPreview();
  drawCursor();
}

void drawToolPanel() {
  TFT_eSPI& d = *tft;
  d.fillRect(0, 0, W, PANEL_H, PANEL_BG);
  d.drawFastHLine(0, PANEL_H, W, TFT_WHITE);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextDatum(MC_DATUM);
  for (uint8_t i = 0; i < TOOL_COUNT; ++i) {
    const int16_t x = 2 + i * 45;
    const bool sel = i == static_cast<uint8_t>(st.tool);
    d.fillRoundRect(x, 3, 43, 16, 3, sel ? TFT_YELLOW : PANEL_CHIP);
    d.setTextColor(sel ? TFT_BLACK : TFT_WHITE, sel ? TFT_YELLOW : PANEL_CHIP);
    d.drawString(TOOL_NAMES[i], x + 21, 11);
  }

  char label[32];
  optionLabel(label, sizeof(label), st.tool,
              st.option[static_cast<uint8_t>(st.tool)]);
  d.setTextDatum(ML_DATUM);
  d.setTextColor(TFT_WHITE, PANEL_BG);
  d.setTextSize(1);
  d.drawString(label, 4, 28);
  d.drawString("Color:", 230, 28);
  d.fillRect(268, 22, 48, 13, lut[color]);
  d.drawRect(267, 21, 50, 15, TFT_WHITE);
}

void drawToolHint(uint32_t exitHeld) {
  TFT_eSPI& d = *tft;
  d.fillRect(0, 38, W, PANEL_H - 39, PANEL_BG);
  if (exitHeld > 0) {
    const int16_t full = W - 40;
    d.drawRect(19, 40, full + 2, 8, TFT_WHITE);
    d.fillRect(20, 41, full * min<uint32_t>(exitHeld, EXIT_HOLD_MS) /
                           EXIT_HOLD_MS, 6, TFT_YELLOW);
    return;
  }
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextDatum(MC_DATUM);
  d.setTextColor(0xB5B6, PANEL_BG);
  d.drawString("1 Tool  2 Size  3 Color  Dbl1 Close  Hold4 Exit", W / 2, 43);
}

void drawMapCell(int8_t cx, int8_t cy, bool selected) {
  TFT_eSPI& d = *tft;
  const int16_t x = MAP_X + cx * MAP_CELL, y = MAP_Y + cy * MAP_CELL;
  d.fillRect(x, y, MAP_CELL, MAP_CELL, lut[palette[cy * MAP_N + cx]]);
  if (selected) {
    d.drawRect(x, y, MAP_CELL, MAP_CELL, TFT_WHITE);
    d.drawRect(x + 1, y + 1, MAP_CELL - 2, MAP_CELL - 2, TFT_BLACK);
  }
}

void drawColorPreview() {
  TFT_eSPI& d = *tft;
  const uint8_t c = palette[st.cellY * MAP_N + st.cellX];
  d.fillRect(222, 30, 90, 56, lut[c]);
  d.drawRect(221, 29, 92, 58, TFT_WHITE);
  d.fillRect(222, 92, 90, 30, lut[color]);
  d.drawRect(221, 91, 92, 32, TFT_WHITE);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextDatum(TL_DATUM);
  d.setTextColor(0xB5B6, TFT_BLACK);
  d.drawString("New", 222, 20);
  d.drawString("Current", 222, 126 - 2);
}

void drawColorScreen() {
  TFT_eSPI& d = *tft;
  d.fillScreen(TFT_BLACK);
  d.setTextFont(1);
  d.setTextSize(2);
  d.setTextDatum(TL_DATUM);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Pick a color", MAP_X, 4);
  for (int8_t y = 0; y < MAP_N; ++y)
    for (int8_t x = 0; x < MAP_N; ++x)
      drawMapCell(x, y, x == st.cellX && y == st.cellY);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("1 Left  2 Up", 222, 150);
  d.drawString("3 Down  4 Right", 222, 164);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString("Dbl 1: pick", 222, 186);
  d.drawString("Dbl 2: cancel", 222, 200);
  drawColorPreview();
}

void showHint() {
  TFT_eSPI& d = *tft;
  d.fillRect(0, H - 16, W, 16, TFT_BLACK);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextDatum(MC_DATUM);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("Dbl 2: pen on/off    Dbl 1: tools", W / 2, H - 8);
}

// ------------------------------------------------------------ mode logic

void clampPen() {
  st.px = constrain(st.px, 0, W - 1);
  st.py = constrain(st.py, 0, H - 1);
}

void liftPen() {
  if (!penDown) return;
  if (isShape(st.tool)) commitShape();
  penDown = false;
  flushDirty();
}

void togglePen() {
  if (penDown) {
    liftPen();
  } else if (st.tool == Tool::Fill) {
    floodFill(st.px, st.py);
    flushDirty();
  } else {
    penDown = true;
    anchorX = st.px;
    anchorY = st.py;
    if (!isShape(st.tool)) {
      stampAt(st.px, st.py);
      flushDirty();
    }
  }
  updateOverlays();
}

void moveCursor(int dx, int dy) {
  const int16_t ox = st.px, oy = st.py;
  st.px += dx;
  st.py += dy;
  clampPen();
  if (st.px == ox && st.py == oy) return;
  if (penDown && !isShape(st.tool)) {
    strokeTo(ox, oy, st.px, st.py);
    flushDirty();
  }
  updateOverlays();
}

void enterTools() {
  liftPen();
  mode = Mode::Tools;
  updateOverlays();
  drawToolPanel();
  drawToolHint(0);
}

void leaveTools() {
  mode = Mode::Draw;
  blit(0, 0, W - 1, PANEL_H);
  updateOverlays();
}

void enterColor() {
  mode = Mode::Color;
  drawColorScreen();
}

void leaveColor() {
  mode = Mode::Tools;
  blit(0, 0, W - 1, H - 1);
  drawToolPanel();
  drawToolHint(0);
}

void moveColorCell(int dx, int dy) {
  const int8_t nx = constrain(st.cellX + dx, 0, MAP_N - 1);
  const int8_t ny = constrain(st.cellY + dy, 0, MAP_N - 1);
  if (nx == st.cellX && ny == st.cellY) return;
  drawMapCell(st.cellX, st.cellY, false);
  st.cellX = nx;
  st.cellY = ny;
  drawMapCell(st.cellX, st.cellY, true);
  drawColorPreview();
}

void dirOf(Button b, int& dx, int& dy) {
  dx = dy = 0;
  if (b == Button::Button1) dx = -1;
  else if (b == Button::Button2) dy = -1;
  else if (b == Button::Button3) dy = 1;
  else if (b == Button::Button4) dx = 1;
}

bool hasDoubleAction(Button b) {
  switch (mode) {
    case Mode::Draw: return b == Button::Button1 || b == Button::Button2;
    case Mode::Tools: return b == Button::Button1;
    default: return b == Button::Button1 || b == Button::Button2;
  }
}

void singlePress(Button b) {
  int dx, dy;
  dirOf(b, dx, dy);
  if (mode == Mode::Draw) {
    moveCursor(dx, dy);
  } else if (mode == Mode::Color) {
    moveColorCell(dx, dy);
  } else {
    const uint8_t tool = static_cast<uint8_t>(st.tool);
    if (b == Button::Button1) {
      st.tool = static_cast<Tool>((tool + 1) % TOOL_COUNT);
      drawToolPanel();
    } else if (b == Button::Button2) {
      uint8_t& o = st.option[tool];
      o = (o + 1) % optionCount(st.tool);
      drawToolPanel();
    } else if (b == Button::Button3) {
      st.cellX = st.cellY = 0;
      for (int i = 0; i < MAP_N * MAP_N; ++i) {
        if (palette[i] == color) {
          st.cellX = i % MAP_N;
          st.cellY = i / MAP_N;
          break;
        }
      }
      enterColor();
    }
  }
}

void doublePress(Button b) {
  if (mode == Mode::Draw) {
    if (b == Button::Button2) togglePen();
    else enterTools();
  } else if (mode == Mode::Tools) {
    leaveTools();
  } else if (b == Button::Button1) {
    color = palette[st.cellY * MAP_N + st.cellX];
    leaveColor();
  } else {
    leaveColor();
  }
}
}  // namespace

void drawGalleryIcon(TFT_eSPI& display, int16_t x, int16_t y) {
  display.fillRect(x + 2, y + 2, 72, 40, 0x7BCF);
  display.fillRect(x + 6, y + 6, 64, 32, TFT_WHITE);
  display.fillRect(x + 6, y + 26, 64, 12, TFT_GREEN);
  display.fillTriangle(x + 14, y + 30, x + 30, y + 12, x + 46, y + 30,
                       0x7BEF);
  display.fillCircle(x + 56, y + 14, 5, TFT_YELLOW);
  display.drawLine(x + 8, y + 36, x + 40, y + 33, TFT_RED);
  display.drawLine(x + 8, y + 37, x + 40, y + 34, TFT_RED);
  display.drawRect(x + 2, y + 2, 72, 40, TFT_BLACK);
}

void runGallery(TFT_eSPI& display) {
  tft = &display;
  const bool oldSwap = display.getSwapBytes();
  display.setSwapBytes(true);  // pushImage wants byte-swapped RGB565
  waitForButtonRelease();

  canvas = static_cast<uint8_t*>(malloc(W * H));
  if (!canvas) {
    display.fillScreen(TFT_BLACK);
    display.setTextFont(1);
    display.setTextSize(2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(MC_DATUM);
    display.drawString("Not enough memory", W / 2, H / 2);
    delay(1500);
    display.setSwapBytes(oldSwap);
    return;
  }
  memset(canvas, WHITE332, W * H);
  buildLut();
  buildPalette();
  resetDirty();

  memset(&st, 0, sizeof(st));
  st.px = W / 2;
  st.py = H / 2;
  mode = Mode::Draw;
  color = BLACK332;
  penDown = false;
  oldCursor.valid = oldPreview.valid = false;

  blit(0, 0, W - 1, H - 1);
  updateOverlays();
  showHint();
  uint32_t hintUntil = millis() + HINT_MS;
  bool hintShown = true;

  // Input state.
  Button held = Button::None;
  uint32_t pressTime = 0, lastRepeat = 0, lastBar = 0;
  bool repeatOk = false;
  Button tapButton = Button::None;
  uint32_t tapTime = 0;
  Mode tapMode = Mode::Draw;
  bool tapValid = false;
  State beforePress = st;
  bool pressWasDouble = false;
  bool exiting = false;

  while (!exiting) {
    const Button b = readButton();
    const uint32_t now = millis();

    if (b != held) {
      if (held != Button::None) {  // release
        tapValid = !pressWasDouble && held == tapButton &&
                   now - tapTime < TAP_MAX_MS;
        if (mode == Mode::Tools && held == Button::Button4) drawToolHint(0);
      }
      held = b;
      if (b != Button::None) {  // press
        if (hintShown) {
          blit(0, H - 16, W - 1, H - 1);
          updateOverlays();
          hintShown = false;
        }
        pressTime = lastRepeat = now;
        repeatOk = true;
        const bool dbl = hasDoubleAction(b) && tapValid && tapButton == b &&
                         tapMode == mode && now - tapTime <= DOUBLE_TAP_MS;
        pressWasDouble = dbl;
        if (dbl) {
          tapValid = false;
          repeatOk = false;
          // Undo what the first tap of the pair did, then do the double.
          st = beforePress;
          if (mode == Mode::Draw) {
            clampPen();
            updateOverlays();
          }
          doublePress(b);
        } else {
          beforePress = st;
          tapButton = b;
          tapTime = now;
          tapMode = mode;
          tapValid = false;
          singlePress(b);
        }
      }
    } else if (b != Button::None) {
      // Held: auto-repeat movement, or the exit countdown in tool mode.
      if (mode == Mode::Tools && b == Button::Button4) {
        const uint32_t heldMs = now - pressTime;
        if (heldMs >= EXIT_HOLD_MS) {
          exiting = true;
        } else if (heldMs > 250 && now - lastBar >= 60) {
          lastBar = now;
          drawToolHint(heldMs);
        }
      } else if (repeatOk && mode != Mode::Tools) {
        const uint32_t heldMs = now - pressTime;
        const uint32_t every =
            mode == Mode::Draw ? DRAW_REPEAT_MS : COLOR_REPEAT_MS;
        if (heldMs >= REPEAT_DELAY_MS && now - lastRepeat >= every) {
          lastRepeat = now;
          int dx, dy;
          dirOf(b, dx, dy);
          if (mode == Mode::Draw) {
            const int step = min<uint32_t>(
                4, 1 + (heldMs - REPEAT_DELAY_MS) / 800);
            moveCursor(dx * step, dy * step);
          } else {
            moveColorCell(dx, dy);
          }
        }
      }
    }

    if (hintShown && now > hintUntil) {
      blit(0, H - 16, W - 1, H - 1);
      updateOverlays();
      hintShown = false;
    }
    delay(4);
  }

  waitForButtonRelease();
  free(canvas);
  canvas = nullptr;
  display.setSwapBytes(oldSwap);
}

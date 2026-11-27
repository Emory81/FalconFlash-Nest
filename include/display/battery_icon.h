#pragma once

#include <TFT_eSPI.h>

#include <cstdio>

#include "power/battery_monitor.h"

// Same battery glyph the Desk Mode clock uses, with the percent and the
// smoothed voltage beside it. percent < 0 draws nothing.
inline void drawBatteryIndicator(TFT_eSPI& canvas, int8_t percent,
                                 uint16_t background) {
  const int16_t x = 12;
  const int16_t y = 9;
  const int16_t bodyW = 22;
  const int16_t bodyH = 11;
  // Ends at x = 99 so the menu's left scroll arrow (starts near x = 98) is
  // never cleared.
  canvas.fillRect(x - 2, y - 3, 90, bodyH + 6, background);
  if (percent < 0) {
    return;
  }
  const uint16_t color = percent <= 15   ? TFT_RED
                         : percent <= 30 ? TFT_YELLOW
                                         : TFT_GREEN;
  canvas.drawRoundRect(x, y, bodyW, bodyH, 2, TFT_LIGHTGREY);
  canvas.fillRect(x + bodyW, y + 3, 2, bodyH - 6, TFT_LIGHTGREY);
  const int16_t fillW = ((bodyW - 4) * percent) / 100;
  if (fillW > 0) {
    canvas.fillRect(x + 2, y + 2, fillW, bodyH - 4, color);
  }
  const uint16_t millivolts = batteryMilliVolts();
  char text[16];
  if (millivolts > 0) {
    snprintf(text, sizeof(text), "%d%% %u.%02uV", percent, millivolts / 1000,
             (millivolts % 1000) / 10);
  } else {
    snprintf(text, sizeof(text), "%d%%", percent);
  }
  canvas.setTextFont(1);
  canvas.setTextSize(1);
  canvas.setTextDatum(ML_DATUM);
  canvas.setTextColor(TFT_WHITE, background);
  canvas.drawString(text, x + bodyW + 6, y + bodyH / 2);
}

#include "power_down_sound.h"

#include <Arduino.h>

#include "audio_helper.h"

void playPowerDownSound() {
  audioBegin();

  // Descending three-note shutdown chime.
  audioPlayTone(988, 100);
  delay(25);
  audioPlayTone(659, 120);
  delay(25);
  audioPlayTone(392, 220);
  audioEnd();
}

#include "boot_sound.h"

#include <Arduino.h>

#include "audio_helper.h"

void playBootSound() {
  audioBegin();

  // Short, loud four-note "ba-da-da-bum" startup fanfare.
  audioPlayTone(659, 90);
  delay(25);
  audioPlayTone(784, 90);
  delay(25);
  audioPlayTone(988, 100);
  delay(25);
  audioPlayTone(1319, 240);
  audioEnd();
}

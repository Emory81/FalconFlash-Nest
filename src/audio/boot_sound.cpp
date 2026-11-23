#include "audio/boot_sound.h"

#include <Arduino.h>

#include "audio/audio_helper.h"

void playBootSound() {
  audioBegin();

  // Short, loud four-note "ba-da-da-bum" startup fanfare.
  // Kept tight so wake-to-timer stays near ~3s.
  audioPlayTone(659, 55);
  delay(15);
  audioPlayTone(784, 55);
  delay(15);
  audioPlayTone(988, 65);
  delay(15);
  audioPlayTone(1319, 140);
  audioEnd();
}

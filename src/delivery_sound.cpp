#include "delivery_sound.h"

#include <Arduino.h>

#include "audio_helper.h"

void playDeliverySound() {
  audioBegin();

  // Loud two-note doorbell-style "ding-dong".
  audioPlayTone(988, 180);
  delay(45);
  audioPlayTone(659, 300);
  audioEnd();
}

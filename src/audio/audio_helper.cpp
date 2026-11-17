#include "audio/audio_helper.h"

#include <Arduino.h>

namespace {
// Temporarily silences every sound (boot, delivery, power-down, games).
// Set to false to turn sound back on.
constexpr bool AUDIO_MUTED = true;
constexpr uint8_t SPEAKER_ENABLE_PIN = 4;
constexpr uint8_t AUDIO_DAC_PIN = 26;
constexpr uint8_t DAC_MIN = 0;
constexpr uint8_t DAC_MAX = 255;
constexpr uint16_t SPEAKER_SETTLE_MS = 80;
}  // namespace

void audioBegin() {
  pinMode(SPEAKER_ENABLE_PIN, OUTPUT);
  digitalWrite(SPEAKER_ENABLE_PIN, LOW);
  dacWrite(AUDIO_DAC_PIN, DAC_MIN);
  delay(SPEAKER_SETTLE_MS);
}

void audioPlayTone(uint16_t frequency, uint16_t durationMs) {
  if (AUDIO_MUTED) {
    return;
  }

  const uint32_t halfPeriodUs = 500000UL / frequency;
  const uint32_t cycles =
      (static_cast<uint32_t>(frequency) * durationMs) / 1000UL;

  for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
    dacWrite(AUDIO_DAC_PIN, DAC_MAX);
    delayMicroseconds(halfPeriodUs);
    dacWrite(AUDIO_DAC_PIN, DAC_MIN);
    delayMicroseconds(halfPeriodUs);
  }
}

void audioEnd() {
  dacWrite(AUDIO_DAC_PIN, DAC_MIN);
  digitalWrite(SPEAKER_ENABLE_PIN, HIGH);
}

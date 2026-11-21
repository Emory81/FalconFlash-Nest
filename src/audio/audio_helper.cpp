#include "audio/audio_helper.h"

#include <Arduino.h>

#include "device_config.h"

namespace {
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
  if (deviceConfig().audioMuted) {
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

void playMessageNotificationSound() {
  if (deviceConfig().audioMuted) {
    return;
  }

  audioBegin();
  // A short rising three-note chirp approximates a Snapchat-style alert
  // using the device's existing DAC tone helper.
  audioPlayTone(1047, 55);
  delay(20);
  audioPlayTone(1319, 55);
  delay(20);
  audioPlayTone(1568, 100);
  audioEnd();
}

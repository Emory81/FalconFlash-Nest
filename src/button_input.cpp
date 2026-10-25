#include "button_input.h"

#include <Arduino.h>

namespace {
constexpr uint8_t BUTTON_ADC_PIN = 35;
constexpr uint8_t ADC_SAMPLE_COUNT = 8;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;

// These boundaries sit between the nominal ADC values in the resistor ladder:
// BTN1=0, BTN2~=370, BTN3~=740, BTN4~=1016, and released~=4095.
constexpr int BUTTON1_MAX = 160;
constexpr int BUTTON2_MAX = 550;
// Keep the boundary below the BTN4 level measured on the assembled ladder.
constexpr int BUTTON3_MAX = 800;
// Leave extra room for resistor tolerance and the ESP32 ADC's attenuation
// error while keeping the released 3.3 V rail outside this range.
constexpr int BUTTON4_MAX = 2400;

Button classifyReading(int reading) {
  if (reading <= BUTTON1_MAX) {
    return Button::Button1;
  }
  if (reading <= BUTTON2_MAX) {
    return Button::Button2;
  }
  if (reading <= BUTTON3_MAX) {
    return Button::Button3;
  }
  if (reading <= BUTTON4_MAX) {
    return Button::Button4;
  }
  return Button::None;
}

int readAveragedAdc() {
  uint32_t total = 0;
  for (uint8_t sample = 0; sample < ADC_SAMPLE_COUNT; ++sample) {
    total += analogRead(BUTTON_ADC_PIN);
  }
  return static_cast<int>(total / ADC_SAMPLE_COUNT);
}
}  // namespace

void initButtons() {
  pinMode(BUTTON_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(BUTTON_ADC_PIN, ADC_11db);
}

Button readButton() {
  static Button stableButton = Button::None;
  static Button candidateButton = Button::None;
  static uint32_t candidateSince = 0;

  const Button rawButton = classifyReading(readAveragedAdc());
  const uint32_t now = millis();
  if (rawButton != candidateButton) {
    candidateButton = rawButton;
    candidateSince = now;
  }

  if (candidateButton != stableButton &&
      now - candidateSince >= BUTTON_DEBOUNCE_MS) {
    stableButton = candidateButton;
  }
  return stableButton;
}

Button readButtonEvent() {
  static Button reportedButton = Button::None;
  const Button currentButton = readButton();

  if (currentButton == Button::None) {
    reportedButton = Button::None;
    return Button::None;
  }
  if (currentButton != reportedButton) {
    reportedButton = currentButton;
    return currentButton;
  }
  return Button::None;
}

void waitForButtonRelease() {
  while (readButton() != Button::None) {
    delay(5);
  }
}

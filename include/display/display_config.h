#pragma once

// Landscape orientation: 90 degrees counter-clockwise from portrait.
// Logical resolution after rotation: 320 x 240.
constexpr uint8_t DISPLAY_ROTATION = 3;

// Physical controls use the ADC resistor ladder on GPIO35:
// BTN1/BTN2=menu movement, BTN3=select/retry, BTN4=back.
// Games may use BTN1/BTN2 for gameplay actions.

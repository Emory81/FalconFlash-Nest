#pragma once

#include <stdint.h>

enum class Button : uint8_t {
  None = 0,
  Button1,
  Button2,
  Button3,
  Button4,
};

void initButtons();
Button readButton();
Button readButtonEvent();
void waitForButtonRelease();

#pragma once

#include <stdint.h>

void audioBegin(bool settleSpeaker = true);
void audioPlayTone(uint16_t frequency, uint16_t durationMs);
void audioEnd();

#pragma once

#include <stdint.h>

void audioBegin();
void audioPlayTone(uint16_t frequency, uint16_t durationMs);
void audioEnd();
void playMessageNotificationSound();

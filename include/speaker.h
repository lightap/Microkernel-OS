#ifndef SPEAKER_H
#define SPEAKER_H

#include "types.h"

void speaker_beep(uint32_t frequency, uint32_t duration_ms);
void speaker_play_note(uint32_t freq, uint32_t ms);
void speaker_startup_sound(void);

#endif

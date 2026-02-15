#ifndef TIMER_H
#define TIMER_H

#include "types.h"
#include <stdint.h>



void     timer_init(uint32_t frequency);
uint32_t timer_get_ticks(void);
uint32_t timer_get_seconds(void);
uint32_t timer_get_frequency(void);
void     timer_get_uptime(uint32_t* hours, uint32_t* mins, uint32_t* secs);
void     timer_sleep(uint32_t ms);

#endif

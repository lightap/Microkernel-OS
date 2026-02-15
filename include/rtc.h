#ifndef RTC_H
#define RTC_H

#include "types.h"

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
    uint8_t weekday;
} rtc_time_t;

void rtc_init(void);
void rtc_read(rtc_time_t* time);
void rtc_print_time(void);
void rtc_print_date(void);

#endif

#include "rtc.h"
#include "vga.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int is_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (bcd & 0x0F) + ((bcd >> 4) * 10);
}

void rtc_init(void) {
    /* Nothing special needed, CMOS is always available */
}

void rtc_read(rtc_time_t* t) {
    while (is_updating());

    t->second  = cmos_read(0x00);
    t->minute  = cmos_read(0x02);
    t->hour    = cmos_read(0x04);
    t->day     = cmos_read(0x07);
    t->month   = cmos_read(0x08);
    t->year    = cmos_read(0x09);
    t->weekday = cmos_read(0x06);

    uint8_t regB = cmos_read(0x0B);

    /* Convert BCD to binary if needed */
    if (!(regB & 0x04)) {
        t->second  = bcd_to_bin(t->second);
        t->minute  = bcd_to_bin(t->minute);
        t->hour    = bcd_to_bin(t->hour & 0x7F) | (t->hour & 0x80);
        t->day     = bcd_to_bin(t->day);
        t->month   = bcd_to_bin(t->month);
        t->year    = bcd_to_bin(t->year);
    }

    /* Convert 12h to 24h */
    if (!(regB & 0x02) && (t->hour & 0x80)) {
        t->hour = ((t->hour & 0x7F) + 12) % 24;
    }

    t->year += 2000; /* Assume 21st century */
}

static const char* weekday_names[] = {
    "???", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char* month_names[] = {
    "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

void rtc_print_time(void) {
    rtc_time_t t;
    rtc_read(&t);
    kprintf("%d:%d:%d", t.hour, t.minute, t.second);
}

void rtc_print_date(void) {
    rtc_time_t t;
    rtc_read(&t);
    kprintf("%s %s %u %u", weekday_names[t.weekday % 8],
            month_names[t.month <= 12 ? t.month : 0], t.day, t.year);
}

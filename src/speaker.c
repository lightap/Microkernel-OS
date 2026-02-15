#include "speaker.h"
#include "timer.h"

static void speaker_on(uint32_t frequency) {
    uint32_t divisor = 1193180 / frequency;
    outb(0x43, 0xB6);  /* PIT channel 2, mode 3 */
    outb(0x42, (uint8_t)(divisor & 0xFF));
    outb(0x42, (uint8_t)(divisor >> 8));

    uint8_t val = inb(0x61);
    if ((val & 3) != 3)
        outb(0x61, val | 3);
}

static void speaker_off(void) {
    outb(0x61, inb(0x61) & ~3);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_on(frequency);
    timer_sleep(duration_ms);
    speaker_off();
}

void speaker_play_note(uint32_t freq, uint32_t ms) {
    speaker_beep(freq, ms);
    timer_sleep(20); /* Small gap between notes */
}

void speaker_startup_sound(void) {
    /* Quick ascending chirp */
    speaker_play_note(523, 60);  /* C5 */
    speaker_play_note(659, 60);  /* E5 */
    speaker_play_note(784, 80);  /* G5 */
    speaker_play_note(1047, 120); /* C6 */
}

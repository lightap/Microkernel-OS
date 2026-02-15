#ifndef MOUSE_H
#define MOUSE_H

#include "types.h"

void mouse_init(void);
void mouse_set_bounds(int max_x, int max_y);
int  mouse_get_x(void);
int  mouse_get_y(void);
bool mouse_left_held(void);
bool mouse_right_held(void);
bool mouse_left_click(void);   /* Edge-triggered: true once per press */
bool mouse_right_click(void);
void mouse_set_bounds(int max_x, int max_y);

/* Poll for input events (call regularly from GUI loop).
 * Processes virtio-input events if PS/2 mouse is inactive. */
void mouse_poll(void);

#endif

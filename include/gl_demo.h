#ifndef GL_DEMO_H
#define GL_DEMO_H

/* Launch fullscreen OpenGL demo (ESC to exit) */
void gl_demo_start(void);

/* 3D shape drawing functions (call between glBegin/glEnd GL_QUADS) */
void gl_draw_cube(float size);
void gl_draw_pyramid(float size);
void gl_draw_torus(float R, float r, int rings, int sides);
void gl_draw_sphere(float radius, int slices, int stacks);
void gl_draw_floor(void);
void gl_draw_axes(void);

#endif

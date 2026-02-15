#ifndef MINIGL_H
#define MINIGL_H

#include "types.h"

/* ---- Primitive types ---- */
#define GL_TRIANGLES     0x0004
#define GL_QUADS         0x0007
#define GL_TRIANGLE_FAN  0x0006
#define GL_TRIANGLE_STRIP 0x0005
#define GL_LINES         0x0001
#define GL_LINE_STRIP    0x0003
#define GL_POLYGON       0x0009

/* ---- Matrix modes ---- */
#define GL_MODELVIEW     0x1700
#define GL_PROJECTION    0x1701

/* ---- Enable/disable caps ---- */
#define GL_DEPTH_TEST    0x0B71
#define GL_LIGHTING      0x0B50
#define GL_LIGHT0        0x4000
#define GL_CULL_FACE     0x0B44
#define GL_COLOR_MATERIAL 0x0B57

/* ---- Polygon mode ---- */
#define GL_LINE          0x1B01
#define GL_FILL          0x1B02

/* ---- Light parameters ---- */
#define GL_POSITION      0x1203
#define GL_AMBIENT       0x1200
#define GL_DIFFUSE       0x1201
#define GL_SPECULAR      0x1202

/* ---- Clear bits ---- */
#define GL_COLOR_BUFFER_BIT  0x4000
#define GL_DEPTH_BUFFER_BIT  0x0100

/* ---- Init / shutdown ---- */
void glInit(uint32_t* framebuffer, uint16_t width, uint16_t height);
void glSetTarget(uint32_t* framebuffer, uint16_t width, uint16_t height);
void glClose(void);

/* ---- State ---- */
void glEnable(int cap);
void glDisable(int cap);
void glPolygonMode(int mode);
void glClear(int mask);
void glClearColor(float r, float g, float b, float a);
void glViewport(int x, int y, int w, int h);

/* ---- Matrix ops ---- */
void glMatrixMode(int mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(float x, float y, float z);
void glRotatef(float angle, float x, float y, float z);
void glScalef(float x, float y, float z);
void glMultMatrixf(const float* m);
void gluPerspective(float fovy, float aspect, float znear, float zfar);
void glOrtho(float l, float r, float b, float t, float n, float f);
void gluLookAt(float ex, float ey, float ez,
               float cx, float cy, float cz,
               float ux, float uy, float uz);

/* ---- Drawing ---- */
void glBegin(int mode);
void glEnd(void);
void glVertex3f(float x, float y, float z);
void glVertex2f(float x, float y);
void glColor3f(float r, float g, float b);
void glColor4f(float r, float g, float b, float a);
void glNormal3f(float nx, float ny, float nz);

/* ---- Lighting ---- */
void glLightfv(int light, int param, const float* values);

/* ---- Swap / present ---- */
void glSwapBuffers(void);

/* ---- Utility ---- */
float gl_sin(float x);
float gl_cos(float x);
float gl_tan(float x);
float gl_sqrt(float x);

#endif

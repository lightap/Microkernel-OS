#include "userlib.h"

#define PI 3.14159265f

/* Vertex layout: X, Y, Z, W, R, G, B, A (8 floats) */
typedef struct { float m[16]; } mat4_t;

/* ---- 3D Math (row-major) ---- */

static void mat4_identity(mat4_t* M) {
    for (int i = 0; i < 16; i++) M->m[i] = 0.0f;
    M->m[0] = M->m[5] = M->m[10] = M->m[15] = 1.0f;
}

/* out = A * B */
static void mat4_mul(mat4_t* out, const mat4_t* A, const mat4_t* B) {
    mat4_t R;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            R.m[r*4+c] = A->m[r*4+0] * B->m[0*4+c] +
                         A->m[r*4+1] * B->m[1*4+c] +
                         A->m[r*4+2] * B->m[2*4+c] +
                         A->m[r*4+3] * B->m[3*4+c];
        }
    }
    *out = R;
}

static void mat4_perspective(mat4_t* M, float fovy, float aspect, float zn, float zf) {
    float rad = fovy * PI / 180.0f;
    float f = u_cos(rad / 2.0f) / u_sin(rad / 2.0f);
    mat4_identity(M);
    M->m[0]  = f / aspect;
    M->m[5]  = f;
    M->m[10] = (zf + zn) / (zn - zf);
    M->m[11] = (2.0f * zf * zn) / (zn - zf);
    M->m[14] = -1.0f;
    M->m[15] = 0.0f;
}

static void mat4_translate(mat4_t* M, float x, float y, float z) {
    mat4_identity(M);
    M->m[3]  = x;
    M->m[7]  = y;
    M->m[11] = z;
}

static void mat4_rotate_y(mat4_t* M, float a) {
    float c = u_cos(a), s = u_sin(a);
    mat4_identity(M);
    M->m[0] = c;  M->m[2]  = s;
    M->m[8] = -s; M->m[10] = c;
}

static void mat4_rotate_x(mat4_t* M, float a) {
    float c = u_cos(a), s = u_sin(a);
    mat4_identity(M);
    M->m[5]  = c;  M->m[6]  = -s;
    M->m[9]  = s;  M->m[10] = c;
}

/* ---- Cube Geometry ---- */

#define V(px,py,pz, cr,cg,cb)  px,py,pz,1.0f, cr,cg,cb,1.0f

static float gpu_cube_verts[] = {
    /* Front face */
    V(-1,-1, 1,  1.0f,0.0f,0.0f),
    V( 1,-1, 1,  0.8f,0.2f,0.0f),
    V( 1, 1, 1,  1.0f,0.5f,0.0f),
    V(-1,-1, 1,  1.0f,0.0f,0.0f),
    V( 1, 1, 1,  1.0f,0.5f,0.0f),
    V(-1, 1, 1,  0.8f,0.1f,0.2f),

    /* Back face */
    V( 1,-1,-1,  0.0f,0.8f,0.8f),
    V(-1,-1,-1,  0.0f,0.5f,1.0f),
    V(-1, 1,-1,  0.0f,1.0f,0.8f),
    V( 1,-1,-1,  0.0f,0.8f,0.8f),
    V(-1, 1,-1,  0.0f,1.0f,0.8f),
    V( 1, 1,-1,  0.2f,0.8f,1.0f),

    /* Left face */
    V(-1,-1,-1,  0.0f,0.6f,0.0f),
    V(-1,-1, 1,  0.2f,1.0f,0.2f),
    V(-1, 1, 1,  0.0f,0.8f,0.4f),
    V(-1,-1,-1,  0.0f,0.6f,0.0f),
    V(-1, 1, 1,  0.0f,0.8f,0.4f),
    V(-1, 1,-1,  0.1f,1.0f,0.1f),

    /* Right face */
    V( 1,-1, 1,  0.0f,0.2f,1.0f),
    V( 1,-1,-1,  0.2f,0.0f,0.8f),
    V( 1, 1,-1,  0.4f,0.2f,1.0f),
    V( 1,-1, 1,  0.0f,0.2f,1.0f),
    V( 1, 1,-1,  0.4f,0.2f,1.0f),
    V( 1, 1, 1,  0.2f,0.5f,1.0f),

    /* Top face */
    V(-1, 1, 1,  1.0f,1.0f,0.0f),
    V( 1, 1, 1,  1.0f,0.8f,0.2f),
    V( 1, 1,-1,  0.8f,1.0f,0.0f),
    V(-1, 1, 1,  1.0f,1.0f,0.0f),
    V( 1, 1,-1,  0.8f,1.0f,0.0f),
    V(-1, 1,-1,  1.0f,0.9f,0.3f),

    /* Bottom face */
    V(-1,-1,-1,  0.8f,0.0f,0.8f),
    V( 1,-1,-1,  1.0f,0.2f,0.6f),
    V( 1,-1, 1,  0.6f,0.0f,1.0f),
    V(-1,-1,-1,  0.8f,0.0f,0.8f),
    V( 1,-1, 1,  0.6f,0.0f,1.0f),
    V(-1,-1, 1,  1.0f,0.0f,1.0f),
};

#define GPU_NUM_VERTS  36

/* ---- Draw a grid of cubes (repeat cube with slight per-cube rotX/rotY) ---- */

static void draw_cube_instance(const mat4_t* proj,
                               float tx, float ty, float tz,
                               float rotX_a, float rotY_a,
                               float scale)
{
    mat4_t T, RX, RY, MV, TMP, S, MVP;

    /* Scale (simple diagonal matrix) */
    mat4_identity(&S);
    S.m[0]  = scale;
    S.m[5]  = scale;
    S.m[10] = scale;

    mat4_translate(&T, tx, ty, tz);
    mat4_rotate_x(&RX, rotX_a);
    mat4_rotate_y(&RY, rotY_a);

    /* MV = T * RX * RY * S  (order matches the rest of your code style) */
    mat4_mul(&TMP, &T,  &RX);
    mat4_mul(&MV,  &TMP, &RY);
    mat4_mul(&MV,  &MV,  &S);

    mat4_mul(&MVP, proj, &MV);

    float xformed[GPU_NUM_VERTS * 8];
    for (int i = 0; i < GPU_NUM_VERTS; i++) {
        const float* src = &gpu_cube_verts[i * 8];
        float*       dst = &xformed[i * 8];

        /* Transform XYZW */
        for (int row = 0; row < 4; row++) {
            float v = 0.0f;
            for (int col = 0; col < 4; col++)
                v += MVP.m[row*4 + col] * src[col];
            dst[row] = v;
        }

        /* Copy RGBA */
        dst[4] = src[4];
        dst[5] = src[5];
        dst[6] = src[6];
        dst[7] = src[7];
    }

    sys_gpu3d_upload(xformed, GPU_NUM_VERTS * 8);
    sys_gpu3d_draw(GPU3D_TRIANGLES, 0, GPU_NUM_VERTS);
}

static void gpu_render_loop(void) {
    float t = 0.0f;

    /* Grid settings */
    const int   GRID_X = 4;
    const int   GRID_Y = 4;
    const float SPACING = 8.2f;  /* distance between cube centers */
    const float CUBE_SCALE = 0.85f;

    while (1) {
        mat4_t proj;
        mat4_perspective(&proj, 60.0f, 16.0f/9.0f, 0.1f, 300.0f);

        sys_gpu3d_clear(GPU3D_CLEAR_COLOR | GPU3D_CLEAR_DEPTH, 0xFF0A0A14);

        /* Center the grid around origin */
        float half_w = (GRID_X - 1) * SPACING * 0.5f;
        float half_h = (GRID_Y - 1) * SPACING * 0.5f;

        for (int y = 0; y < GRID_Y; y++) {
            for (int x = 0; x < GRID_X; x++) {
                float px = (x * SPACING) - half_w;
                float py = (y * SPACING) - half_h;
                float pz = -35.0f; /* push everything into view */

                /* Slight per-cube rotation offsets + global motion */
                float rx = 0.25f + (0.08f * (float)y) + (0.03f * (float)x) + (t * 0.03f);
                float ry = 0.55f + (0.10f * (float)x) + (0.02f * (float)y) + (t * 0.05f);

                draw_cube_instance(&proj, px, py, pz, rx, ry, CUBE_SCALE);
            }
        }

        sys_gpu3d_present();

        t += 1.0f;
        /* sys_sleep(16); */
    }
}

int main(void) {
    sys_debug_log("GPU Cubes Grid: Initializing...\n");

    if (!sys_gui_win_open("3D Cubes Grid")) {
        sys_exit(1);
    }

    if (sys_gpu3d_init(1280, 720) != 0) {
        sys_debug_log("GPU3D Init Failed\n");
        sys_exit(1);
    }

    gpu_render_loop();
    return 0;
}

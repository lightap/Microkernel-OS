# VirtIO-GPU 3D (Virgl) Integration Guide

## Your Current Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Your app (gui.c, gl_demo.c)              │
│   glBegin(GL_TRIANGLES); glVertex3f(...); glEnd();          │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│               minigl.c — SOFTWARE rasterizer (CPU)          │
│                                                             │
│  transform_vertex()  →  rasterize_triangle()  →  pixels    │
│  Matrix math            Edge function loop       Writes to  │
│  Projection             Z-buffer test            gl_pixbuf  │
│  Lighting               Gouraud interpolation               │
└────────────────────────┬────────────────────────────────────┘
                         │ memcpy
                         ▼
┌─────────────────────────────────────────────────────────────┐
│              gui.c blit() — copies pixels to display        │
│                                                             │
│  BGA backend:    write to framebuffer at 0x20000000 (MMIO)  │
│  VirtIO backend: copy to virtio FB → transfer → flush       │
└─────────────────────────────────────────────────────────────┘
```

**The problem:** minigl.c does all rendering on the CPU. Your virtio-gpu driver
is just a 2D display pipe — it shows pixels you've already computed. At
1920×1080, clearing + Z-buffering + rasterizing large triangles is slow.


## What GPU Acceleration Looks Like

```
┌─────────────────────────────────────────────────────────────┐
│                    Your app (gui.c, gl_demo.c)              │
│   glBegin(GL_TRIANGLES); glVertex3f(...); glEnd();          │
└────────────────────────┬────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────┐
│            virgl_gl_bridge.c — translates to GPU commands   │
│                                                             │
│  glEnd() → pack vertices → compute MVP → build cmd buffer  │
│            (CPU just packs data, doesn't rasterize)         │
└────────────────────────┬────────────────────────────────────┘
                         │ virgl_cmd_submit()
                         ▼
┌─────────────────────────────────────────────────────────────┐
│             virgl.c — sends command buffer via virtio        │
│                                                             │
│  VIRTIO_GPU_CMD_SUBMIT_3D  →  virtio control queue          │
└────────────────────────┬────────────────────────────────────┘
                         │ virtio ring (DMA)
                         ▼
┌─────────────────────────────────────────────────────────────┐
│    HOST: QEMU + virglrenderer  →  HOST GPU (real hardware)  │
│                                                             │
│  Deserializes virgl commands → translates to OpenGL/Vulkan  │
│  GPU rasterizes at hardware speed → renders to scanout      │
└─────────────────────────────────────────────────────────────┘
```

## The Three Approaches (Pick One)

### Approach 1: Quick Win — Optimize the 2D display path (1 hour)

This doesn't add GPU rendering, but fixes the biggest bottleneck you have
RIGHT NOW: the double-copy in blit(). Your current path:

    minigl renders to gl_pixbuf     (CPU write)
    blit() copies to backbuf        (CPU memcpy — wasted!)
    blit() copies to virtio FB      (CPU memcpy)
    virtio_gpu_flush_all()          (full-screen transfer — slow!)

**Fix: Render directly to the virtio FB and use dirty rectangles.**

In gui.c, change the 3D window drawing to:

```c
/* Instead of: */
glInit(gl_pixbuf, GL_WIN_W, GL_WIN_H);
/* ... render scene to gl_pixbuf ... */
/* ... then blit gl_pixbuf into backbuf at window position ... */

/* Do this: */
/* Render minigl directly into the window region of backbuf */
glSetTarget(backbuf + (win_y * GFX_W + win_x), GL_WIN_W, GL_WIN_H);
/* After rendering, flush only the window rectangle: */
if (using_virtio_gpu) {
    uint32_t* fb = virtio_gpu_get_fb();
    /* Copy just the window region */
    for (int row = 0; row < win_h; row++) {
        memcpy(&fb[(win_y + row) * GFX_W + win_x],
               &backbuf[(win_y + row) * GFX_W + win_x],
               win_w * 4);
    }
    /* Flush only the dirty rectangle */
    virtio_gpu_flush(win_x, win_y, win_w, win_h);
}
```

This alone can give you 2-5× speedup for small 3D windows.


### Approach 2: Virgl 3D — Real GPU acceleration (days/weeks)

This is the full path. The files I've provided give you:

- `virgl.h` — Protocol definitions and API
- `virgl.c` — Transport layer (context creation, command submission)
- `virgl_gl_bridge.c` — Translates your gl* calls to virgl commands

**What works immediately:**
- Feature negotiation (detecting virgl support)
- Context creation
- Resource allocation (framebuffer, depth buffer, VBO)
- Command buffer building (clear, viewport, draw_vbo)
- Submitting command buffers to the host

**What you still need to implement:**
The hardest part: **creating GPU pipeline state objects** (shaders, blend,
rasterizer, DSA, vertex elements, surfaces). These require encoding Gallium3D
state in the virgl wire format, which is byte-level protocol work.

Specifically:

1. **Vertex shader** (TGSI format) — transforms vertices by MVP matrix
2. **Fragment shader** (TGSI format) — outputs interpolated vertex color
3. **Blend state** — no blending, write RGBA
4. **Rasterizer state** — filled triangles, no face culling
5. **Depth-stencil state** — depth test enabled, write depth
6. **Vertex elements** — describes your vertex layout (pos3f + color4f)
7. **Surface objects** — wraps color/depth textures for framebuffer

Each of these is a CREATE_OBJECT command with a specific binary layout.

**To learn the exact encoding:** Study virglrenderer's source code:
- `src/vrend_decode.c` — decodes the wire format (tells you exact layout)
- `src/virgl_protocol.h` — defines all the structs and constants


### Approach 3: Hybrid — GPU display, CPU render (recommended first step)

Keep minigl.c for rendering (it works and is debugged), but use the
virtio-gpu **2D** path more efficiently:

1. Render to a buffer with minigl (CPU)
2. Use virtio-gpu RESOURCE_CREATE_2D + TRANSFER + FLUSH efficiently
3. Use double-buffering: render to back buffer while front is displayed
4. Only transfer dirty regions

This gives you a working 60fps display pipeline. Then add virgl 3D later.


## Step-By-Step Integration Plan

### Step 1: Modify virtio.c for feature negotiation

```c
/* In virtio.h, add: */
bool virtio_init_features(virtio_dev_t* dev, uint16_t pci_device_id,
                          uint32_t wanted_features);

/* In virtio.c, around line 237: */
/* Replace the hardcoded feature acceptance with: */
uint32_t accepted = features & wanted_features;
mmio_write32(dev->common_cfg, VIRTIO_COMMON_GF, accepted);
serial_printf("virtio: accepted features[0]=%x\n", accepted);
```

### Step 2: Detect virgl at boot

```c
/* In kernel.c, after virtio_gpu_init(): */
if (virtio_gpu_available()) {
    /* Check if 3D is available */
    bool has_3d = virgl_available();
    kprintf("  [OK] VirtIO-GPU (%s)\n", has_3d ? "3D virgl" : "2D only");
}
```

### Step 3: Add virgl files to Makefile

Your Makefile already uses wildcard for src/*.c, so just drop
virgl.c and virgl_gl_bridge.c into src/ and add the header to include/.

### Step 4: QEMU command line

```bash
# 2D only (what you use now):
qemu-system-i386 -device virtio-gpu-pci ...

# 3D virgl (required for GPU acceleration):
qemu-system-i386 -device virtio-gpu-gl-pci -display gtk,gl=on \
    -m 2G -kernel microkernel.bin \
    -drive file=disk.img,format=raw \
    -drive file=ntfs.img,format=raw \
    -serial stdio
```

### Step 5: Runtime dispatch in gui.c

```c
static bool use_virgl_gl = false;

/* In gui startup: */
if (virgl_available()) {
    use_virgl_gl = virgl_gl_init(GL_WIN_W, GL_WIN_H);
    if (use_virgl_gl) {
        serial_printf("GUI: using GPU-accelerated rendering\n");
    }
}
if (!use_virgl_gl) {
    /* Fallback to software */
    gl_pixbuf = kmalloc(GL_WIN_W * GL_WIN_H * 4);
    glInit(gl_pixbuf, GL_WIN_W, GL_WIN_H);
}
```


## File Summary

```
include/virgl.h           — Protocol definitions, command types, API
src/virgl.c               — VirtIO-GPU 3D transport layer
src/virgl_gl_bridge.c     — minigl API → virgl GPU command translation
```

These files are designed to coexist with your existing minigl.c and
virtio_gpu.c. The bridge provides virgl_gl* versions of every gl* function,
and you can switch between CPU and GPU rendering at runtime.


## Realistic Expectations

| What                      | Difficulty | Speedup        |
|---------------------------|------------|----------------|
| Dirty rect 2D flush       | Easy       | 2-5× for GUI   |
| Direct-to-FB rendering    | Easy       | Eliminates copy |
| virgl context + resources | Medium     | Foundation only |
| virgl pipeline state      | Hard       | Required for 3D |
| virgl TGSI shaders        | Hard       | Required for 3D |
| Full virgl rendering      | Very Hard  | 10-100×         |

The virgl pipeline state encoding is the main barrier. Once you have that
working, the actual rendering commands (clear, draw, flush) are
straightforward — and the code I've provided handles those.

For reference: Mesa's virgl Gallium driver (the Linux implementation) is
~15,000 lines. You don't need all of it — a minimal vertex-color renderer
needs maybe 500-800 lines of pipeline setup code. But those 500 lines require
understanding the virgl wire format intimately.

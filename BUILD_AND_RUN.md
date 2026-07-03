# Complete project — microkernel with GPU-accelerated compiz desktop
# (as of round 19 of the GPU work)

## This IS the exact tree that produced the included microkernel.bin
Everything is here: src/, include/, Makefile, linker.ld, user.ld,
boot files, prebuilt build/*.elf and microkernel.bin. A clean
`make` from this tree reproduces the shipped binary. If your local
compile behaved differently before, it was almost certainly a stale
object file or a partially-copied source — always `make clean` first.

## Build (needs i686-linux-gnu-gcc + nasm; Ubuntu:
##   sudo apt install gcc-i686-linux-gnu nasm)
    make clean
    make                 # -> microkernel.bin
    make hello amazing texcube    # -> build/*.elf

## Run (disk images are YOUR existing ntfs.img / disk.img — not included)
    qemu-system-i386 -enable-kvm -kernel microkernel.bin -m 2G \
      -vga none -device virtio-gpu-gl-pci -display gtk,gl=on \
      -device virtio-tablet-pci \
      -hda ntfs.img -hdb disk.img \
      -netdev user,id=n0 -device rtl8139,netdev=n0 \
      -serial stdio -initrd build/hello.elf

## In the shell
    compiz          -> GPU-drawn desktop (SAFE: proven pipeline only)
    compiz wobble   -> same + arms wobbly-window texturing (diagnostic;
                       if it black-screens, capture host stderr + serial)
    gui             -> classic 2D desktop (VGA/BGA preferred)

## Desktop quick reference
  - ` (backtick)  toggles display orientation if it ever comes up flipped
  - keyboard goes to the FOCUSED window: click a titlebar first
  - hello.elf under compiz renders inside a draggable window
    (own virgl sub-context); under classic gui it takes the fullscreen
  - serial prints: compiz FPS/sec, gpu3d FPS/sec, "gui: key=..." per key

## Where the GPU work lives
  src/virtio_gpu.c   2D framebuffer path, restore-scanout, no fb size cap
  src/virtio.c       spin-poll virtio_wait, wait statistics
  src/virgl.c        virgl driver: compiz compositor, GPU scene renderer,
                     dirty-band uploads, flip pipeline, sub-contexts,
                     windowed-app lifecycle, submit lock
  src/virgl_pipeline.c  pipe state; texture-sampling setup (wobble, opt-in)
  src/gui.c          desktop; compiz scene builder, wobbly-window spring
                     physics (wob_*), windowed-GL compositing
  src/syscall.c      GPU3D syscalls: windowed init under compiz, batched
                     present, FPS/perf reporter
  src/mouse.c        virtio-tablet authoritative over stray PS/2 bytes

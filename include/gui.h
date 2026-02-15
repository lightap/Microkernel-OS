#ifndef GUI_H
#define GUI_H

#include "types.h"

void gui_start(void);

/* ---- ELF process GUI window support ---- */

/* Shared framebuffer dimensions for ELF GUI windows */
#define ELF_GUI_FB_W    320
#define ELF_GUI_FB_H    200
#define ELF_GUI_FB_UVADDR  0xA0000000  /* User-space VA for framebuffer */

/*
 * Open a GUI window for the calling ELF process.
 * Allocates a shared framebuffer mapped into the process's address space.
 * Returns user-space framebuffer virtual address, or 0 on failure.
 */
uint32_t gui_elf_win_open(uint32_t pid, uint32_t* page_dir, const char* title);

/*
 * Present the framebuffer (no-op, buffer is read each frame).
 */
void gui_elf_win_present(uint32_t pid);

/*
 * Close the GUI window owned by the given PID.
 */
void gui_elf_win_close(uint32_t pid);

/*
 * Called when an ELF process exits — cleans up any GUI window it owns.
 */
void gui_elf_process_exited(uint32_t pid);

#endif

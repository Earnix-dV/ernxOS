#ifndef ERNXOS_GFX_H
#define ERNXOS_GFX_H

#include <stdint.h>

/* ---------------- VGA Mode 13h (320x200x256) graphics + graphical desktop ---------------- */

#define GFX_WIDTH  320
#define GFX_HEIGHT 200

/* graphics-mode desktop state - referenced by mouse.c (mouse_handle_byte
   branches on gfx_active/gfx_dirty and updates gfx_mouse_x/y) as well as
   by gfx.c itself. */
extern volatile int gfx_active;   /* 1 while cmd_gfx's desktop loop is running */
extern volatile int gfx_dirty;    /* set by mouse moves/clicks; cmd_gfx redraws and clears it */
extern int gfx_mouse_x, gfx_mouse_y; /* center of the 320x200 screen */

/* GUI actions are queued by the mouse ISR and executed by cmd_gfx's main
   loop. Never run the shell or ERNXscript interpreter from IRQ context. */
#define GFX_ACTION_NONE  0
#define GFX_ACTION_SHELL 1
#define GFX_ACTION_EDIT  2
#define GFX_ACTION_GAME  3
extern volatile int gfx_pending_action;
extern char gfx_pending_file[32];

/* Call once at boot, while still in the BIOS/GRUB-provided text mode and
   before 'gfx' or the graphical desktop have ever run, so the real font
   is captured before anything can clobber it. */
void vga_save_text_font(void);

/* Bresenham's line algorithm - general-purpose graphics primitive,
   reserved for future desktop drawing (not yet used by any window). */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color);

/* called by mouse.c's mouse_handle_byte when a click lands while the
   graphics desktop owns the screen. */
void gfx_handle_click(int x, int y);

/* `gfx` shell command - the real graphics desktop. */
void cmd_gfx(void);

#endif /* ERNXOS_GFX_H */

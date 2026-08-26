#include "gfx.h"
#include "io.h"
#include "vga.h"
#include "util.h"
#include "vfs.h"
#include "wm.h"
#include "keyboard.h"
#include "ernxscript.h"

/* graphics-mode desktop state - see gfx.h for why these live here even
   though mouse.c also touches them directly. */
volatile int gfx_active = 0;
volatile int gfx_dirty = 0;
int gfx_mouse_x = 160, gfx_mouse_y = 100;
volatile int gfx_pending_action = GFX_ACTION_NONE;
char gfx_pending_file[32];

/* ---------------- VGA Mode 13h (320x200x256) graphics ----------------
   After GRUB hands off, real-mode BIOS interrupts (INT 10h) are gone, so
   switching video modes means reprogramming the VGA registers by hand -
   there's no BIOS left to call. The register tables below are the
   standard values for mode 0x13 (320x200, 256 colors, linear framebuffer
   at 0xA0000) and for mode 0x03 (80x25 text, what the rest of this
   kernel assumes), so `gfx` can drop into graphics mode and come back
   to a working text console afterward. */

static uint8_t* const VGA_GFX_MEMORY = (uint8_t*) 0xA0000;

#define VGA_AC_INDEX    0x3C0
#define VGA_AC_WRITE    0x3C0
#define VGA_SEQ_INDEX   0x3C4
#define VGA_SEQ_DATA    0x3C5
#define VGA_GC_INDEX    0x3CE
#define VGA_GC_DATA     0x3CF
#define VGA_CRTC_INDEX  0x3D4
#define VGA_CRTC_DATA   0x3D5
#define VGA_MISC_WRITE  0x3C2
#define VGA_INSTAT_READ 0x3DA

static const uint8_t g_mode13_misc      = 0x63;
static const uint8_t g_mode13_seq[5]    = {0x03, 0x01, 0x0F, 0x00, 0x0E};
static const uint8_t g_mode13_crtc[25]  = {
    0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,
    0x00,0x00,0x00,0x9C,0x0E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF
};
static const uint8_t g_mode13_gc[9]     = {0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF};
static const uint8_t g_mode13_ac[21]    = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,
    0x0D,0x0E,0x0F,0x41,0x00,0x0F,0x00,0x00
};

static const uint8_t g_text03_misc      = 0x67;
static const uint8_t g_text03_seq[5]    = {0x03, 0x00, 0x03, 0x00, 0x02};
static const uint8_t g_text03_crtc[25]  = {
    0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,
    0x00,0x00,0x50,0x9C,0x0E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF
};
static const uint8_t g_text03_gc[9]     = {0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF};
static const uint8_t g_text03_ac[21]    = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,
    0x3D,0x3E,0x3F,0x0C,0x00,0x0F,0x08,0x00
};

/* shared by both mode switches - only the register tables differ */
static void gfx_load_registers(uint8_t misc, const uint8_t* seq, const uint8_t* crtc,
                                const uint8_t* gc, const uint8_t* ac) {
    /* VGA register programming is order-sensitive.  In particular, the
       sequencer must be put into synchronous reset while its registers are
       changed.  Without that, switching from Mode 13h back to text mode can
       leave the display hardware in a partially programmed state. */
    outb(VGA_SEQ_INDEX, 0x00);
    outb(VGA_SEQ_DATA, 0x01); /* synchronous reset */

    outb(VGA_MISC_WRITE, misc);

    for (uint8_t i = 1; i < 5; i++) {
        outb(VGA_SEQ_INDEX, i);
        outb(VGA_SEQ_DATA, seq[i]);
    }

    /* CRTC registers 0-7 are write-protected by register 0x11.  Unlock
       register 0x11 before writing the complete CRTC table. */
    outb(VGA_CRTC_INDEX, 0x11);
    uint8_t crtc11 = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_INDEX, 0x11);
    outb(VGA_CRTC_DATA, (uint8_t)(crtc11 & 0x7F));

    for (uint8_t i = 0; i < 25; i++) {
        outb(VGA_CRTC_INDEX, i);
        outb(VGA_CRTC_DATA, crtc[i]);
    }

    for (uint8_t i = 0; i < 9; i++) {
        outb(VGA_GC_INDEX, i);
        outb(VGA_GC_DATA, gc[i]);
    }

    /* Attribute Controller uses one port for both index and data.  Reading
       the input-status register resets its internal flip-flop to index mode. */
    (void) inb(VGA_INSTAT_READ);
    for (uint8_t i = 0; i < 21; i++) {
        outb(VGA_AC_INDEX, i);
        outb(VGA_AC_WRITE, ac[i]);
    }
    outb(VGA_AC_INDEX, 0x20); /* enable display/palette output */

    /* Program sequencer register 0 last to leave reset and start the new
       mode cleanly. */
    outb(VGA_SEQ_INDEX, 0x00);
    outb(VGA_SEQ_DATA, seq[0]);
}

static void gfx_enter_mode13h(void) {
    gfx_load_registers(g_mode13_misc, g_mode13_seq, g_mode13_crtc, g_mode13_gc, g_mode13_ac);
}

/* ---------------- Text-mode character generator (font) save/restore ----
   Mode 13h reprograms the sequencer/graphics controller into chain-4,
   linear addressing and treats the whole 0xA0000-0xAFFFF window as one
   flat pixel buffer. But that is the SAME physical VRAM that, in text
   mode, holds the character bitmaps in plane 2 (loaded once by the
   BIOS/GRUB before the kernel took over the CPU - there's no BIOS left
   afterward to reload them). Drawing anything in graphics mode - icons,
   windows, the cursor - overwrites plane 2 with pixel garbage. Coming
   back to text mode then reprograms the CRTC/sequencer correctly, but
   every glyph the text console draws now reads corrupted bitmap data:
   the character *codes* in plane 0 are fine, so the cursor moves and
   input works, but every letter renders as stray stripes/lines instead
   of text. Saving font bank 0 once at boot (before graphics mode is
   ever entered) and rewriting it into plane 2 on every return to text
   mode fixes this. */

#define VGA_FONT_BYTES 8192 /* font bank 0: 256 chars * 32-byte plane stride */
static uint8_t g_text_font_backup[VGA_FONT_BYTES];
static int g_font_backed_up = 0;

static void vga_font_plane_io(uint8_t* buf, int do_write) {
    uint8_t seq2, seq4, gc4, gc5, gc6;
    volatile uint8_t* mem = (volatile uint8_t*) 0xA0000;

    /* Save the registers we're about to repurpose so we can put the
       caller's mode (whichever it was) back exactly as it was. */
    outb(VGA_SEQ_INDEX, 0x02); seq2 = inb(VGA_SEQ_DATA);
    outb(VGA_SEQ_INDEX, 0x04); seq4 = inb(VGA_SEQ_DATA);
    outb(VGA_GC_INDEX, 0x04);  gc4  = inb(VGA_GC_DATA);
    outb(VGA_GC_INDEX, 0x05);  gc5  = inb(VGA_GC_DATA);
    outb(VGA_GC_INDEX, 0x06);  gc6  = inb(VGA_GC_DATA);

    /* Point straight at plane 2, sequential (non-chain-4) addressing, so
       0xA0000 maps directly onto the font bitmap bytes. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, 0x04); /* map mask: plane 2 only */
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, 0x07); /* sequential, extended memory, no chain-4 */
    outb(VGA_GC_INDEX, 0x04);  outb(VGA_GC_DATA, 0x02);  /* read map select: plane 2 */
    outb(VGA_GC_INDEX, 0x05);  outb(VGA_GC_DATA, 0x00);  /* read/write mode 0, no odd/even */
    outb(VGA_GC_INDEX, 0x06);  outb(VGA_GC_DATA, 0x04);  /* graphics addressing, map at 0xA0000-0xAFFFF */

    if (do_write) {
        for (int i = 0; i < VGA_FONT_BYTES; i++) mem[i] = buf[i];
    } else {
        for (int i = 0; i < VGA_FONT_BYTES; i++) buf[i] = mem[i];
    }

    /* Restore whatever addressing mode was active before this call. */
    outb(VGA_SEQ_INDEX, 0x02); outb(VGA_SEQ_DATA, seq2);
    outb(VGA_SEQ_INDEX, 0x04); outb(VGA_SEQ_DATA, seq4);
    outb(VGA_GC_INDEX, 0x04);  outb(VGA_GC_DATA, gc4);
    outb(VGA_GC_INDEX, 0x05);  outb(VGA_GC_DATA, gc5);
    outb(VGA_GC_INDEX, 0x06);  outb(VGA_GC_DATA, gc6);
}

/* Call once at boot, while still in the BIOS/GRUB-provided text mode and
   before 'gfx' or the graphical desktop have ever run, so the real font
   is captured before anything can clobber it. */
void vga_save_text_font(void) {
    vga_font_plane_io(g_text_font_backup, 0);
    g_font_backed_up = 1;
}

/* Call after every return to text mode (ESC out of the desktop, the
   TERMINAL launcher, a game finishing, etc.) to undo whatever graphics
   mode did to plane 2. */
static void vga_restore_text_font(void) {
    if (!g_font_backed_up) return; /* nothing captured yet - leave hardware alone */
    vga_font_plane_io(g_text_font_backup, 1);
}

static void gfx_exit_to_text(void) {
    gfx_load_registers(g_text03_misc, g_text03_seq, g_text03_crtc, g_text03_gc, g_text03_ac);
    vga_restore_text_font();

    /* The text console owns B8000 again.  Clear it immediately after the
       mode switch so stale graphics/text data cannot be mistaken for the
       terminal contents. */
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
        }
    }
    term_row = 0;
    term_col = 0;
    update_cursor(0, 0);
}

/* ---- drawing primitives - everything else (windows, icons, cursor) is
   built out of these four ---- */

static inline void gfx_put_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    VGA_GFX_MEMORY[y * GFX_WIDTH + x] = color;
}

static void gfx_clear(uint8_t color) {
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) VGA_GFX_MEMORY[i] = color;
}

static void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    int x0 = x, x1 = x + w - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= GFX_WIDTH) x1 = GFX_WIDTH - 1;
    for (int row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int px = x0; px <= x1; px++) {
            VGA_GFX_MEMORY[py * GFX_WIDTH + px] = color;
        }
    }
}

/* unfilled rectangle - four 1px-thick edges reusing fill_rect, so it
   stays correctly clipped for free */
static void gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    if (w <= 0 || h <= 0) return;
    gfx_fill_rect(x, y, w, 1, color);
    gfx_fill_rect(x, y + h - 1, w, 1, color);
    gfx_fill_rect(x, y, 1, h, color);
    gfx_fill_rect(x + w - 1, y, 1, h, color);
}

/* Bresenham's line algorithm - handles all 8 octants via sx/sy */
void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* ---- 8x8 bitmap text - everything else that shows text in graphics
   mode (window titles, buttons, icon labels) is built on gfx_draw_string ---- */

static const char gfx_font_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,:;!?'\"-+=()/_*><";

/* 8x8 bitmap font, one row per byte (bit7=leftmost pixel).
   Covers space, A-Z, 0-9, and basic punctuation used in UI text.
   Index into gfx_font8x8[] via gfx_font_index(c). */
static const uint8_t gfx_font8x8[55][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x18,0x24,0x42,0x7E,0x42,0x42,0x42,0x00}, /* 'A' */
    {0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00}, /* 'B' */
    {0x3E,0x40,0x40,0x40,0x40,0x40,0x3E,0x00}, /* 'C' */
    {0x7C,0x42,0x42,0x42,0x42,0x42,0x7C,0x00}, /* 'D' */
    {0x7E,0x40,0x40,0x7C,0x40,0x40,0x7E,0x00}, /* 'E' */
    {0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x00}, /* 'F' */
    {0x3E,0x40,0x40,0x4E,0x42,0x42,0x3E,0x00}, /* 'G' */
    {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00}, /* 'H' */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 'I' */
    {0x1E,0x04,0x04,0x04,0x44,0x44,0x38,0x00}, /* 'J' */
    {0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00}, /* 'K' */
    {0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00}, /* 'L' */
    {0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00}, /* 'M' */
    {0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00}, /* 'N' */
    {0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /* 'O' */
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00}, /* 'P' */
    {0x3C,0x42,0x42,0x42,0x4A,0x44,0x3C,0x00}, /* 'Q' */
    {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00}, /* 'R' */
    {0x3E,0x40,0x40,0x3C,0x04,0x04,0x7C,0x00}, /* 'S' */
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, /* 'T' */
    {0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00}, /* 'U' */
    {0x42,0x42,0x42,0x42,0x24,0x24,0x18,0x00}, /* 'V' */
    {0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00}, /* 'W' */
    {0x42,0x24,0x18,0x18,0x18,0x24,0x42,0x00}, /* 'X' */
    {0x42,0x42,0x24,0x18,0x18,0x18,0x18,0x00}, /* 'Y' */
    {0x7E,0x04,0x08,0x10,0x20,0x40,0x7E,0x00}, /* 'Z' */
    {0x3C,0x42,0x46,0x4A,0x62,0x42,0x3C,0x00}, /* '0' */
    {0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00}, /* '1' */
    {0x3C,0x42,0x02,0x04,0x08,0x10,0x7E,0x00}, /* '2' */
    {0x7C,0x04,0x08,0x38,0x04,0x04,0x7C,0x00}, /* '3' */
    {0x08,0x18,0x28,0x48,0x7E,0x08,0x08,0x00}, /* '4' */
    {0x7E,0x40,0x7C,0x04,0x04,0x42,0x3C,0x00}, /* '5' */
    {0x3C,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00}, /* '6' */
    {0x7E,0x04,0x08,0x10,0x20,0x20,0x20,0x00}, /* '7' */
    {0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00}, /* '8' */
    {0x3C,0x42,0x42,0x3E,0x04,0x04,0x3C,0x00}, /* '9' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00}, /* '.' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x30}, /* ',' */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, /* ':' */
    {0x00,0x18,0x18,0x00,0x18,0x18,0x20,0x00}, /* ';' */
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, /* '!' */
    {0x3C,0x42,0x02,0x0C,0x08,0x00,0x08,0x00}, /* '?' */
    {0x18,0x18,0x20,0x00,0x00,0x00,0x00,0x00}, /* '\'' */
    {0x24,0x24,0x00,0x00,0x00,0x00,0x00,0x00}, /* '"' */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* '-' */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* '+' */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* '=' */
    {0x08,0x10,0x10,0x10,0x10,0x10,0x08,0x00}, /* '(' */
    {0x20,0x10,0x10,0x10,0x10,0x10,0x20,0x00}, /* ')' */
    {0x04,0x08,0x10,0x20,0x40,0x40,0x00,0x00}, /* '/' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00}, /* '_' */
    {0x00,0x28,0x10,0x7C,0x10,0x28,0x00,0x00}, /* '*' */
    {0x00,0x40,0x20,0x10,0x20,0x40,0x00,0x00}, /* '>' */
    {0x00,0x08,0x10,0x20,0x10,0x08,0x00,0x00}, /* '<' */
};

/* lowercase reuses its uppercase glyph - this font only has one case */
static int gfx_font_index(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (unsigned i = 0; i < sizeof(gfx_font_chars) - 1; i++) {
        if (gfx_font_chars[i] == c) return (int) i;
    }
    return -1; /* unsupported char - caller skips it (renders as a gap) */
}

/* draws one glyph with a transparent background - only 'set' bits are
   plotted, so text can sit on top of a colored bar/rect without
   punching a rectangular hole in it first */
static void gfx_draw_char(int x, int y, char c, uint8_t color) {
    int idx = gfx_font_index(c);
    if (idx < 0) return;
    const uint8_t* rows = gfx_font8x8[idx];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) gfx_put_pixel(x + col, y + row, color);
        }
    }
}

static void gfx_draw_string(int x, int y, const char* str, uint8_t color) {
    int cx = x;
    while (*str) {
        gfx_draw_char(cx, y, *str, color);
        cx += 8;
        str++;
    }
}

/* draws one titled window as a flat rectangle: border, title bar, and
   body fill - the pixel-mode equivalent of the text-mode draw_window()
   used by cmd_win. */
/* Forward declaration used by window content rendering. */
static void gfx_window_rect(int idx, int* px, int* py, int* pw, int* ph);

static void gfx_draw_window(int x, int y, int w, int h, const char* title, uint8_t body_color) {
    const int title_h = 12;
    gfx_fill_rect(x, y, w, title_h, 1);
    gfx_fill_rect(x, y + title_h, w, h - title_h, body_color);
    gfx_draw_rect(x, y, w, h, 15);
    gfx_draw_string(x + 4, y + 2, title, 15);

    /* Tiny close button. */
    gfx_fill_rect(x + w - 11, y + 2, 8, 8, 4);
    gfx_draw_rect(x + w - 11, y + 2, 8, 8, 15);
    gfx_draw_string(x + w - 9, y + 2, "X", 15);
}

static int gfx_is_ernx_name(const char* name) {
    int n = 0;
    while (name[n] != '\0') n++;
    return n >= 5 && name[n-5] == '.' &&
           (name[n-4] == 'e' || name[n-4] == 'E') &&
           (name[n-3] == 'r' || name[n-3] == 'R') &&
           (name[n-2] == 'n' || name[n-2] == 'N') &&
           (name[n-1] == 'x' || name[n-1] == 'X');
}

static int gfx_ernx_at_row(int row) {
    int seen = 0;
    for (int i = 0; i < file_count; i++) {
        if (files[i].is_dir || !gfx_is_ernx_name(files[i].name)) continue;
        if (seen == row) return i;
        seen++;
    }
    return -1;
}

static int gfx_root_file_at_row(int row) {
    int seen = 0;
    for (int i = 0; i < file_count; i++) {
        if (files[i].is_dir || files[i].parent != -1) continue;
        if (seen == row) return i;
        seen++;
    }
    return -1;
}

/* ---- CALC: a small integer calculator, living entirely in the CALC
   window. There's only ever one CALC window (gfx_open_or_focus refuses a
   second), so its state is just a few globals rather than something
   threaded through window_t - the same approach the rest of this desktop
   already takes for GUI state (gfx_mouse_x, gfx_pending_action, ...).
   Only integer arithmetic - nothing in this freestanding kernel has
   floating point support (no libc, no soft-float), so results truncate
   like C integer division (7/2 shows 3, not 3.5). */

#define CALC_DISPLAY_LEN 12
static char calc_display[CALC_DISPLAY_LEN] = "0";
static int32_t calc_operand = 0;
static char calc_pending_op = 0;    /* '+','-','*','/' or 0 for none */
static int calc_new_entry = 1;      /* next digit press replaces the display instead of appending */

static const char* const calc_labels[4][4] = {
    {"7", "8", "9", "/"},
    {"4", "5", "6", "*"},
    {"1", "2", "3", "-"},
    {"C", "0", "=", "+"},
};

/* pixel rect of the row/col button within window idx's content area -
   shared by both the click hit-test and the draw routine so they can
   never disagree about where a button actually is. */
static void gfx_calc_button_rect(int idx, int row, int col, int* bx, int* by, int* bw, int* bh) {
    int x, y, w, h;
    gfx_window_rect(idx, &x, &y, &w, &h);
    const int title_h = 12;
    const int display_h = 16;
    int grid_y = y + title_h + display_h;
    int grid_h = h - title_h - display_h;
    int col_w = w / 4;
    int row_h = grid_h / 4;
    *bx = x + col * col_w;
    *by = grid_y + row * row_h;
    *bw = col_w;
    *bh = row_h;
}

/* applies the pending operator (calc_operand OP display value) and
   leaves the result in both calc_operand and calc_display. Division by
   zero shows "Err" rather than crashing or reading garbage. */
static void gfx_calc_apply_pending(void) {
    int32_t rhs = str_to_int(calc_display);
    int32_t result = 0;
    int err = 0;
    switch (calc_pending_op) {
        case '+': result = calc_operand + rhs; break;
        case '-': result = calc_operand - rhs; break;
        case '*': result = calc_operand * rhs; break;
        case '/':
            if (rhs == 0) err = 1;
            else result = calc_operand / rhs;
            break;
        default: result = rhs; break;
    }
    if (err) {
        calc_display[0] = 'E'; calc_display[1] = 'r'; calc_display[2] = 'r'; calc_display[3] = '\0';
        calc_operand = 0;
    } else {
        int_to_str(result, calc_display);
        calc_operand = result;
    }
    calc_pending_op = 0;
}

/* handles one press on the CALC keypad - digits, an operator, '=', or
   'C' (clear). Called from gfx_handle_click once a click's been narrowed
   down to a specific button. */
static void gfx_calc_press(const char* label) {
    char c = label[0];

    if (c == 'C') {
        calc_display[0] = '0'; calc_display[1] = '\0';
        calc_operand = 0;
        calc_pending_op = 0;
        calc_new_entry = 1;
        return;
    }

    if (c >= '0' && c <= '9') {
        if (calc_new_entry) {
            calc_display[0] = c;
            calc_display[1] = '\0';
            calc_new_entry = 0;
        } else {
            int len = 0;
            while (calc_display[len] != '\0') len++;
            /* leave room for the sign and the null terminator */
            if (len < CALC_DISPLAY_LEN - 2) {
                /* typing more zeroes into a bare "0" would grow it forever
                   without ever changing its value - replace instead */
                if (len == 1 && calc_display[0] == '0') {
                    calc_display[0] = c;
                } else {
                    calc_display[len] = c;
                    calc_display[len + 1] = '\0';
                }
            }
        }
        return;
    }

    if (c == '=') {
        if (calc_pending_op != 0) gfx_calc_apply_pending();
        calc_new_entry = 1;
        return;
    }

    /* + - * / : chain straight into the next operator if one's already
       pending (so "5 + 3 + 2" shows 8 before the second +), otherwise
       just remember the operand and wait for the next number. */
    if (c == '+' || c == '-' || c == '*' || c == '/') {
        if (calc_pending_op != 0 && !calc_new_entry) {
            gfx_calc_apply_pending();
        } else {
            calc_operand = str_to_int(calc_display);
        }
        calc_pending_op = c;
        calc_new_entry = 1;
    }
}

/* hit-tests a click against CALC's 4x4 keypad. Returns 1 (and presses
   the button) if the click landed on one, 0 if it missed the grid
   entirely (e.g. landed on the display instead). */
static int gfx_calc_click(int idx, int x, int y) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int bx, by, bw, bh;
            gfx_calc_button_rect(idx, r, c, &bx, &by, &bw, &bh);
            if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
                gfx_calc_press(calc_labels[r][c]);
                return 1;
            }
        }
    }
    return 0;
}

static void gfx_draw_window_content(int idx) {
    if (idx < 0 || idx >= window_count) return;
    int x, y, w, h;
    gfx_window_rect(idx, &x, &y, &w, &h);
    const window_t* win = &windows[idx];
    int cy = y + 17;

    if (str_eq(win->title, "TERMINAL")) {
        gfx_draw_string(x + 6, cy, "TERMINAL", 15);
        gfx_draw_string(x + 6, cy + 14, "CLICK THE ICON TO ENTER SHELL", 15);
        gfx_draw_string(x + 6, cy + 28, "TYPE COMMANDS, EDIT FILES", 15);
        gfx_draw_string(x + 6, cy + 42, "ESC = RETURN TO DESKTOP", 15);
    } else if (str_eq(win->title, "FILES")) {
        gfx_draw_string(x + 6, cy, "FILES", 15);
        gfx_draw_string(x + 6, cy + 11, "CLICK A FILE TO OPEN", 15);
        int line = 0;
        for (int i = 0; i < file_count && line < 7; i++) {
            if (files[i].is_dir || files[i].parent != -1) continue;
            gfx_draw_string(x + 8, cy + 23 + line * 12, files[i].name, 15);
            line++;
        }
        if (line == 0) gfx_draw_string(x + 8, cy + 25, "NO FILES", 15);
    } else if (str_eq(win->title, "GAMES")) {
        gfx_draw_string(x + 6, cy, "ERNX GAMES", 15);
        gfx_draw_string(x + 6, cy + 11, "CLICK A GAME TO PLAY", 15);
        int line = 0;
        for (int i = 0; i < file_count && line < 7; i++) {
            if (files[i].is_dir || !gfx_is_ernx_name(files[i].name)) continue;
            gfx_draw_string(x + 8, cy + 23 + line * 12, files[i].name, 14);
            line++;
        }
        if (line == 0) gfx_draw_string(x + 8, cy + 25, "NO .ERNX GAMES", 15);
    } else if (str_eq(win->title, "CALC")) {
        const int title_h = 12;
        const int display_h = 16;

        /* screen: dark background, light border, digits right-aligned
           like a real calculator (so new digits appear next to the
           previous ones instead of the number growing off to the right) */
        gfx_fill_rect(x + 2, y + title_h + 2, w - 4, display_h - 4, 0);
        gfx_draw_rect(x + 2, y + title_h + 2, w - 4, display_h - 4, 8);
        int dlen = 0;
        while (calc_display[dlen] != '\0') dlen++;
        int dx = x + w - 4 - dlen * 8;
        if (dx < x + 4) dx = x + 4; /* clamp: never draw left of the screen edge */
        gfx_draw_string(dx, y + title_h + 4, calc_display, 10);

        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                int bx, by, bw, bh;
                gfx_calc_button_rect(idx, r, c, &bx, &by, &bw, &bh);
                uint8_t body = (calc_labels[r][c][0] == 'C') ? 4 : 8; /* clear button stands out in red */
                gfx_fill_rect(bx + 1, by + 1, bw - 2, bh - 2, body);
                gfx_draw_rect(bx + 1, by + 1, bw - 2, bh - 2, 15);
                gfx_draw_string(bx + (bw - 8) / 2, by + (bh - 8) / 2, calc_labels[r][c], 15);
            }
        }
    } else {
        gfx_draw_string(x + 6, cy, "ERNXOS WINDOW", 15);
        gfx_draw_string(x + 6, cy + 12, "CLICK A WINDOW TO FOCUS", 15);
    }
}

/* ---- drawing primitives end ---- */

/* Forward declaration used by window rendering helpers below. */

/* ---- graphics-mode desktop: real windows[], not a static demo ----
   The text-mode screen is 80x25 characters; the pixel screen is 320x200 -
   an exact 4:8 ratio, so a window's existing row/col/width/height (already
   in character cells, set by window_create) convert to a pixel rect with
   a plain multiply. No separate pixel-space window state needed - the
   graphics desktop and the text-mode window manager share one source of
   truth in windows[]. */

static void gfx_window_rect(int idx, int* px, int* py, int* pw, int* ph) {
    window_t* w = &windows[idx];
    *px = w->col * 4;
    *py = w->row * 8;
    *pw = (w->width + 2) * 4;   /* +2: the border columns window_create subtracted */
    *ph = (w->height + 2) * 8;
}

/* click routing for the graphics desktop - the pixel-space counterpart of
   mouse_check_window_click. Focuses the topmost (highest-index, since
   windows are drawn back-to-front) window under (x, y); does nothing if
   the click missed every window. */
static void __attribute__((unused)) gfx_focus_window_at(int x, int y) {
    for (int i = window_count - 1; i >= 0; i--) {
        int px, py, pw, ph;
        gfx_window_rect(i, &px, &py, &pw, &ph);
        if (x >= px && x < px + pw && y >= py && y < py + ph) {
            window_focus(i);
            /* Move the focused window to the front so it behaves like a
               normal desktop window. */
            if (i != window_count - 1) {
                window_t tmp = windows[i];
                for (int j = i; j < window_count - 1; j++) windows[j] = windows[j + 1];
                windows[window_count - 1] = tmp;
                active_window_idx = window_count - 1;
                for (int j = 0; j < window_count; j++) windows[j].active = (j == window_count - 1);
            }
            return;
        }
    }
}

/* Returns 1 when the close button of a graphics window was clicked. */
static int gfx_close_window_at(int x, int y) {
    for (int i = window_count - 1; i >= 0; i--) {
        int px, py, pw, ph;
        gfx_window_rect(i, &px, &py, &pw, &ph);
        if (x >= px + pw - 12 && x < px + pw - 2 && y >= py + 1 && y < py + 9) {
            for (int j = i; j < window_count - 1; j++) windows[j] = windows[j + 1];
            window_count--;
            if (window_count > 0) {
                active_window_idx = window_count - 1;
                for (int j = 0; j < window_count; j++) windows[j].active = (j == active_window_idx);
            } else {
                active_window_idx = 0;
            }
            return 1;
        }
    }
    return 0;
}

/* small filled arrow, black outline / white fill so it stays visible over
   any window or background color underneath it */
static void gfx_draw_cursor(int x, int y) {
    static const char shape[10][9] = {
        "#.......",
        "##......",
        "#O#.....",
        "#OO#....",
        "#OOO#...",
        "#OOOO#..",
        "#OOOOO#.",
        "#OOO####",
        "#O#O#...",
        "#..#OO#.",
    };
    for (int row = 0; row < 10; row++) {
        for (int col = 0; col < 8; col++) {
            char c = shape[row][col];
            if (c == '#') gfx_put_pixel(x + col, y + row, 0);       /* black outline */
            else if (c == 'O') gfx_put_pixel(x + col, y + row, 15); /* white fill */
        }
    }
}

/* ---- taskbar: a strip of per-window buttons plus a "+" button that
   creates a new window without leaving graphics mode - the first bit of
   the desktop that isn't just a mirror of something text-mode already
   had. window_create/window_focus are the same calls `newwin` uses. ---- */

#define TASKBAR_H 16
#define TASKBAR_BTN_W 48
#define TASKBAR_BTN_GAP 2

/* slot == window_count means the "+" button, not a window */
static void gfx_taskbar_button_rect(int slot, int* x, int* y, int* w, int* h) {
    *x = 2 + slot * (TASKBAR_BTN_W + TASKBAR_BTN_GAP);
    *y = GFX_HEIGHT - TASKBAR_H;
    *w = TASKBAR_BTN_W;
    *h = TASKBAR_H;
}

static void gfx_render_taskbar(void) {
    int bar_y = GFX_HEIGHT - TASKBAR_H;
    gfx_fill_rect(0, bar_y, GFX_WIDTH, TASKBAR_H, 8); /* dark gray bar */
    gfx_fill_rect(0, bar_y, GFX_WIDTH, 1, 15);         /* top edge highlight */

    for (int i = 0; i < window_count; i++) {
        int bx, by, bw, bh;
        gfx_taskbar_button_rect(i, &bx, &by, &bw, &bh);
        uint8_t fill = windows[i].active ? 7 : 0;
        gfx_fill_rect(bx, by + 1, bw, bh - 2, fill);
        gfx_draw_rect(bx, by + 1, bw, bh - 2, 15);

        char label[6];
        int n = 0;
        while (windows[i].title[n] != '\0' && n < 5) { label[n] = windows[i].title[n]; n++; }
        label[n] = '\0';
        gfx_draw_string(bx + 2, by + 4, label, windows[i].active ? 0 : 15);
    }

    if (window_count < MAX_WINDOWS) {
        int bx, by, bw, bh;
        gfx_taskbar_button_rect(window_count, &bx, &by, &bw, &bh);
        gfx_fill_rect(bx, by + 1, bw, bh - 2, 2);
        gfx_draw_rect(bx, by + 1, bw, bh - 2, 15);
        gfx_draw_string(bx + bw / 2 - 4, by + 4, "+", 15);
    }
}

/* routes a click to either the taskbar (buttons, or "+" to spawn a new
   window) or, below the taskbar strip, to whichever window it landed on. */
static void gfx_bring_to_front(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_focus(idx);
    if (idx != window_count - 1) {
        window_t tmp = windows[idx];
        for (int j = idx; j < window_count - 1; j++) windows[j] = windows[j + 1];
        windows[window_count - 1] = tmp;
        active_window_idx = window_count - 1;
        for (int j = 0; j < window_count; j++) windows[j].active = (j == window_count - 1);
    }
}

static int gfx_find_window_title(const char* title) {
    for (int i = 0; i < window_count; i++) {
        if (str_eq(windows[i].title, title)) return i;
    }
    return -1;
}

static void gfx_open_or_focus(const char* title) {
    int idx = gfx_find_window_title(title);
    if (idx >= 0) {
        gfx_bring_to_front(idx);
        return;
    }
    if (window_count >= MAX_WINDOWS) return;
    int row = 8, col = 6, height = 11, width = 32;
    if (str_eq(title, "FILES")) { row = 8; col = 44; }
    if (str_eq(title, "GAMES")) { row = 8; col = 23; }
    /* CALC needs a bit more height than the default 11 rows for its
       display + 4x4 keypad to have comfortable button sizes. */
    if (str_eq(title, "CALC")) { row = 6; col = 30; height = 14; width = 30; }
    if (window_create(row, col, height, width, title)) gfx_bring_to_front(window_count - 1);
}

/* routes a click to the taskbar, desktop launchers, window controls, and
   file/game rows.  Long-running work is queued for cmd_gfx's main loop. */
void gfx_handle_click(int x, int y) {
    int taskbar_y = GFX_HEIGHT - TASKBAR_H;
    if (y >= taskbar_y) {
        int slot = (x - 2) / (TASKBAR_BTN_W + TASKBAR_BTN_GAP);
        if (slot >= 0 && slot < window_count) {
            gfx_bring_to_front(slot);
        } else if (slot == window_count && window_count < MAX_WINDOWS) {
            static const char* default_titles[MAX_WINDOWS] = {"WIN1", "WIN2", "WIN3", "WIN4", "WIN5"};
            if (window_create(7 + (window_count % 3), 5 + (window_count % 3) * 10,
                              10, 30, default_titles[window_count])) {
                gfx_bring_to_front(window_count - 1);
            }
        }
        return;
    }

    /* Desktop launchers. */
    if (x >= 8 && x < 48 && y >= 20 && y < 62) {
        gfx_open_or_focus("FILES");
        return;
    }
    if (x >= 52 && x < 96 && y >= 20 && y < 62) {
        gfx_pending_action = GFX_ACTION_SHELL;
        return;
    }
    if (x >= 100 && x < 144 && y >= 20 && y < 62) {
        gfx_open_or_focus("GAMES");
        return;
    }
    if (x >= 148 && x < 192 && y >= 20 && y < 62) {
        gfx_open_or_focus("CALC");
        return;
    }

    if (gfx_close_window_at(x, y)) return;

    /* Game rows: queue the selected .ernx program. */
    for (int i = window_count - 1; i >= 0; i--) {
        int px, py, pw, ph;
        gfx_window_rect(i, &px, &py, &pw, &ph);
        if (x < px || x >= px + pw || y < py || y >= py + ph) continue;
        gfx_bring_to_front(i);
        if (str_eq(windows[window_count - 1].title, "GAMES")) {
            int row = (y - (py + 17) - 23) / 12;
            if (row >= 0) {
                int file_idx = gfx_ernx_at_row(row);
                if (file_idx >= 0) {
                    copy_name(gfx_pending_file, files[file_idx].name);
                    gfx_pending_action = GFX_ACTION_GAME;
                }
            }
        } else if (str_eq(windows[window_count - 1].title, "FILES")) {
            int row = (y - (py + 17) - 23) / 12;
            if (row >= 0) {
                int file_idx = gfx_root_file_at_row(row);
                if (file_idx >= 0) {
                    copy_name(gfx_pending_file, files[file_idx].name);
                    gfx_pending_action = GFX_ACTION_EDIT;
                }
            }
        } else if (str_eq(windows[window_count - 1].title, "CALC")) {
            gfx_calc_click(window_count - 1, x, y);
        }
        return;
    }
}

/* repaints the whole graphics desktop: background, every window
   back-to-front (so higher indices, the more recently focused/created
   ones, land on top), the taskbar, then the cursor last so it's always
   visible. */
static void gfx_render_desktop(void) {
    gfx_clear(1);
    gfx_fill_rect(0, 0, GFX_WIDTH, 14, 9);
    gfx_draw_string(4, 3, "ERNXOS", 15);
    gfx_draw_string(62, 3, "DESKTOP", 15);
    gfx_draw_string(216, 3, "CLICK AN APP", 15);

    /* Desktop launch icons: FILES, TERMINAL, GAMES, CALC. */
    gfx_fill_rect(14, 22, 24, 18, 14);
    gfx_draw_rect(14, 22, 24, 18, 15);
    gfx_draw_string(8, 44, "FILES", 15);

    gfx_fill_rect(60, 22, 24, 18, 7);
    gfx_draw_rect(60, 22, 24, 18, 15);
    gfx_draw_string(52, 44, "TERMINAL", 15);

    gfx_fill_rect(108, 22, 24, 18, 10);
    gfx_draw_rect(108, 22, 24, 18, 15);
    gfx_draw_string(104, 44, "GAMES", 15);

    gfx_fill_rect(156, 22, 24, 18, 5);
    gfx_draw_rect(156, 22, 24, 18, 15);
    gfx_draw_string(152, 44, "CALC", 15);

    for (int i = 0; i < window_count; i++) {
        int px, py, pw, ph;
        gfx_window_rect(i, &px, &py, &pw, &ph);
        uint8_t body = windows[i].active ? 7 : 8;
        gfx_draw_window(px, py, pw, ph, windows[i].title, body);
        gfx_draw_window_content(i);
    }
    if (window_count == 0) {
        gfx_draw_string(86, 88, "NO WINDOWS OPEN", 15);
        gfx_draw_string(70, 100, "USE THE DESKTOP ICONS", 15);
    }
    gfx_render_taskbar();
    gfx_draw_cursor(gfx_mouse_x, gfx_mouse_y);
}


/* `gfx` command - the real graphics desktop: switches to Mode 13h and
   renders the actual windows[] (created via newwin/win, same ones the
   text-mode manager uses), with a live pixel-space cursor. Mouse moves
   and clicks arrive via mouse_handle_byte (which sets gfx_dirty when
   gfx_active is set below) - this loop just waits for either a redraw
   or a keypress, same drain-the-kbd-buffer-directly pattern used by
   ernx_read_line_int, and exits back to text mode on ESC. */
void cmd_gfx(void) {
    terminal_writestring("Entering graphics desktop - click windows to focus, ESC to return.\n");

    gfx_enter_mode13h();
    gfx_active = 1;
    gfx_dirty = 1;

    for (;;) {
        if (gfx_pending_action != GFX_ACTION_NONE) {
            int action = gfx_pending_action;
            gfx_pending_action = GFX_ACTION_NONE;
            gfx_active = 0;
            gfx_exit_to_text();
            terminal_clear();

            if (action == GFX_ACTION_SHELL) {
                terminal_writestring("ERNXOS TERMINAL\n> ");
                return;
            }

            if (action == GFX_ACTION_EDIT) {
                terminal_writestring("Opening file: ");
                terminal_writestring(gfx_pending_file);
                terminal_putchar('\n');
                cmd_edit(gfx_pending_file);
                terminal_writestring("> ");
                return;
            }

            if (action == GFX_ACTION_GAME) {
                char game_name[40];
                int k = 0;
                game_name[k++] = 'r'; game_name[k++] = 'u'; game_name[k++] = 'n'; game_name[k++] = ' ';
                for (int i = 0; gfx_pending_file[i] != '\0' && k < 39; i++) game_name[k++] = gfx_pending_file[i];
                game_name[k] = '\0';
                cmd_run(game_name + 4);
                terminal_writestring("\nGame finished. Returning to desktop...\n");
                gfx_enter_mode13h();
                gfx_active = 1;
                gfx_dirty = 1;
                continue;
            }
        }

        if (gfx_dirty) {
            gfx_render_desktop();
            gfx_dirty = 0;
        }
        if (kbd_buf_tail != kbd_buf_head) {
            uint8_t scancode = kbd_buffer[kbd_buf_tail];
            kbd_buf_tail = (kbd_buf_tail + 1) % KBD_BUF_SIZE;
            if (scancode == 0x01) break; /* ESC (make code) */
            /* Useful desktop shortcuts: T=terminal, F=files, G=games. */
            if (scancode == 0x14) { gfx_pending_action = GFX_ACTION_SHELL; continue; } /* T */
            if (scancode == 0x21) { gfx_open_or_focus("FILES"); gfx_dirty = 1; continue; } /* F */
            if (scancode == 0x22) { gfx_open_or_focus("GAMES"); gfx_dirty = 1; continue; } /* G */
            if (scancode == 0x2E) { gfx_open_or_focus("CALC"); gfx_dirty = 1; continue; } /* C */
        } else {
            __asm__ volatile ("hlt");
        }
    }

    gfx_active = 0;
    gfx_exit_to_text();
    terminal_clear();
    window_redraw_all();
    terminal_writestring("Back to text mode.\n> ");
}


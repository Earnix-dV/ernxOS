#include "mouse.h"
#include "io.h"
#include "vga.h"
#include "util.h"
#include "wm.h"
#include "gfx.h"

#define MOUSE_PORT      0x60
#define MOUSE_STATUS    0x64
#define STATUS_OUT_FULL 0x01 /* data available to read from port 0x60 */
#define STATUS_IN_FULL  0x02 /* controller busy; wait before writing  */
#define STATUS_AUX_BIT  0x20 /* set when the waiting byte is from the mouse */
#define MOUSE_WRITE     0xD4

static void mouse_wait_signal(void) {
    int timeout = 100000;
    while (timeout--) if ((inb(MOUSE_STATUS) & STATUS_IN_FULL) == 0) return;
}

static void mouse_wait_data(void) {
    int timeout = 100000;
    while (timeout--) if (inb(MOUSE_STATUS) & STATUS_OUT_FULL) return;
}

static void mouse_write(uint8_t data) {
    mouse_wait_signal();
    outb(MOUSE_STATUS, MOUSE_WRITE);
    mouse_wait_signal();
    outb(MOUSE_PORT, data);
}

static uint8_t mouse_read(void) {
    mouse_wait_data();
    return inb(MOUSE_PORT);
}

int mouse_x = 40, mouse_y = 12; /* start near center of 80x25 */
static uint8_t mouse_packet[3];
static int mouse_cycle = 0;
static int mouse_left_button_prev = 0; /* track left button state change for clicks */

void mouse_init(void) {
    outb(MOUSE_STATUS, 0xA8);           /* enable auxiliary (mouse) device */

    outb(MOUSE_STATUS, 0x20);           /* read controller command byte */
    mouse_wait_data();
    uint8_t status = inb(MOUSE_PORT) | 0x03; /* enable IRQ1 + IRQ12 */
    status &= ~0x30;                       /* enable keyboard and mouse clocks */

    outb(MOUSE_STATUS, 0x60);           /* write controller command byte */
    mouse_wait_signal();
    outb(MOUSE_PORT, status);

    mouse_write(0xF6);                  /* set defaults */
    mouse_read();                       /* ack */

    mouse_write(0xF4);                  /* enable data reporting (streaming) */
    mouse_read();                       /* ack */
}

void mouse_show_position(void) {
    char buf[32];
    int i = 0;
    buf[i++] = 'M'; buf[i++] = 'o'; buf[i++] = 'u'; buf[i++] = 's';
    buf[i++] = 'e'; buf[i++] = ':'; buf[i++] = ' '; buf[i] = '\0';
    terminal_print_at(24, 0, buf);
    terminal_print_at(24, 8, "X=   Y=   ");
    /* draw digits manually since print_int writes at the scrolling cursor */
    int save_row = term_row, save_col = term_col;
    term_row = 24; term_col = 10;
    print_int(mouse_x);
    term_row = 24; term_col = 16;
    print_int(mouse_y);
    term_row = save_row; term_col = save_col;
}

/* checks if (x, y) is inside window idx. returns 1 if yes, 0 if no. */
int mouse_in_window(int idx, int x, int y) {
    if (idx < 0 || idx >= window_count) return 0;
    window_t* w = &windows[idx];
    return (x >= w->col && x <= w->col + w->width + 1 &&
            y >= w->row && y <= w->row + w->height + 1);
}

/* on left-button click, focus the window under the cursor. */
void mouse_check_window_click(int x, int y) {
    for (int i = 0; i < window_count; i++) {
        if (mouse_in_window(i, x, y)) {
            window_focus(i);
            terminal_clear();
            window_redraw_all();
            terminal_writestring("Focused window: ");
            terminal_writestring(windows[i].title);
            terminal_putchar('\n');
            terminal_writestring("> ");
            return;
        }
    }
}

void mouse_handle_byte(uint8_t data) {
    /* In PS/2 standard 3-byte packets, bit 3 of the first byte is always 1.
       If we ever lose packet alignment, discard bytes until a valid first
       byte appears instead of interpreting arbitrary data as movement/buttons. */
    if (mouse_cycle == 0 && !(data & 0x08)) return;
    mouse_packet[mouse_cycle] = data;
    mouse_cycle++;
    if (mouse_cycle == 3) {
        mouse_cycle = 0;

        /* bits 6/7 of the first byte: X/Y overflow - the real movement didn't
           fit in the signed 8-bit delta, so it can't be trusted. Skip moving
           the cursor this packet rather than risk a garbage jump, but still
           track the button state so a click-and-drag isn't lost. */
        int overflow = (mouse_packet[0] & 0xC0) != 0;
        int dx = 0, dy = 0;
        if (!overflow) {
            dx = (int8_t) mouse_packet[1];
            dy = (int8_t) mouse_packet[2];
        }
        int left_button = (mouse_packet[0] & 0x01) != 0;
        int clicked = left_button && !mouse_left_button_prev;
        mouse_left_button_prev = left_button;

        if (gfx_active) {
            if (dx || dy) {
                gfx_mouse_x += dx / 2;  /* lighter scaling than text mode - full
                                            320px width gives more room to move */
                gfx_mouse_y -= dy / 2;  /* PS/2 Y is inverted vs screen rows */
                if (gfx_mouse_x < 0) gfx_mouse_x = 0;
                if (gfx_mouse_x > GFX_WIDTH - 1) gfx_mouse_x = GFX_WIDTH - 1;
                if (gfx_mouse_y < 0) gfx_mouse_y = 0;
                if (gfx_mouse_y > GFX_HEIGHT - 1) gfx_mouse_y = GFX_HEIGHT - 1;
                gfx_dirty = 1;
            }
            if (clicked) {
                gfx_handle_click(gfx_mouse_x, gfx_mouse_y);
                gfx_dirty = 1;
            }
            return; /* text-mode click/redraw below doesn't apply while the
                        graphics desktop owns the screen */
        }

        if (!overflow) {
            mouse_x += dx / 4;   /* scale down: text cells are big */
            mouse_y -= dy / 4;   /* PS/2 Y is inverted vs screen rows */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x > VGA_WIDTH - 1) mouse_x = VGA_WIDTH - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y > VGA_HEIGHT - 2) mouse_y = VGA_HEIGHT - 2;
        }

        if (clicked && window_count > 0) {
            mouse_check_window_click(mouse_x, mouse_y);
        }

        mouse_show_position();
    }
}

void mouse_irq_handler(registers_t* regs) {
    (void) regs;
    mouse_handle_byte(inb(MOUSE_PORT));
}

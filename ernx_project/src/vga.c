#include "vga.h"
#include "io.h"

/* ---------------- VGA text output ---------------- */

uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;
const int VGA_WIDTH = 80;
const int VGA_HEIGHT = 25;
/* the scrolling terminal only uses rows 0..TERM_HEIGHT-1; the last row is
   reserved for the mouse status line. */
const int TERM_HEIGHT = 25 - 1;

int term_row = 0;
int term_col = 0;
uint8_t term_color = 0x0F; /* white on black */

void terminal_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = 0;
    term_col = 0;
    update_cursor(0, 0);
}

void terminal_scroll(void) {
    for (int y = 1; y < TERM_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[(y - 1) * VGA_WIDTH + x] = VGA_MEMORY[y * VGA_WIDTH + x];
    for (int x = 0; x < VGA_WIDTH; x++)
        VGA_MEMORY[(TERM_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = TERM_HEIGHT - 1;
}

void terminal_putchar(char c) {
    if (c == '\n') {
        term_col = 0;
        term_row++;
    } else if (c == '\b') {
        if (term_col > 0) {
            term_col--;
        } else if (term_row > 0) {
            term_row--;
            term_col = VGA_WIDTH - 1;
        }
        VGA_MEMORY[term_row * VGA_WIDTH + term_col] = vga_entry(' ', term_color);
    } else {
        VGA_MEMORY[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);
        term_col++;
        if (term_col == VGA_WIDTH) {
            term_col = 0;
            term_row++;
        }
    }
    if (term_row >= TERM_HEIGHT) terminal_scroll();
    update_cursor(term_row, term_col);
}

void terminal_writestring(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) terminal_putchar(str[i]);
}

/* print a status line pinned near the bottom, without disturbing the
   normal scrolling cursor (used for mouse coordinates) */
void terminal_print_at(int row, int col, const char* str) {
    int i = 0;
    while (str[i] != '\0' && (col + i) < VGA_WIDTH) {
        VGA_MEMORY[row * VGA_WIDTH + (col + i)] = vga_entry(str[i], term_color);
        i++;
    }
}

/* ---------------- hardware text cursor (the blinking underline) ---------------- */

void update_cursor(int row, int col) {
    uint16_t pos = row * VGA_WIDTH + col;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t) (pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t) ((pos >> 8) & 0xFF));
}

#ifndef ERNXOS_VGA_H
#define ERNXOS_VGA_H

#include <stdint.h>
#include <stddef.h>

/* ---------------- VGA text output ---------------- */

extern uint16_t* const VGA_MEMORY;
extern const int VGA_WIDTH;
extern const int VGA_HEIGHT;
extern const int TERM_HEIGHT;

extern int term_row;
extern int term_col;
extern uint8_t term_color;

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

/* like vga_entry, but for extended/box-drawing bytes (0x80-0xFF) — takes an
   unsigned byte so it doesn't get sign-extended into the color's bits */
static inline uint16_t vga_entry_ext(uint8_t c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

void terminal_clear(void);
void terminal_scroll(void);
void terminal_putchar(char c);
void terminal_writestring(const char* str);
void terminal_print_at(int row, int col, const char* str);

/* ---------------- hardware text cursor (the blinking underline) ---------------- */
void update_cursor(int row, int col);

#endif /* ERNXOS_VGA_H */

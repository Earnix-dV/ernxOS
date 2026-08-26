#ifndef ERNXOS_MOUSE_H
#define ERNXOS_MOUSE_H

#include <stdint.h>
#include "interrupts.h"

/* ---------------- PS/2 mouse ---------------- */

extern int mouse_x, mouse_y;

void mouse_init(void);
void mouse_show_position(void);
int mouse_in_window(int idx, int x, int y);
void mouse_check_window_click(int x, int y);
void mouse_handle_byte(uint8_t data);
void mouse_irq_handler(registers_t* regs);

#endif /* ERNXOS_MOUSE_H */

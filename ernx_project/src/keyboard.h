#ifndef ERNXOS_KEYBOARD_H
#define ERNXOS_KEYBOARD_H

#include <stdint.h>
#include "interrupts.h"

/* ---------------- keyboard (PS/2, scancode set 1) ---------------- */

/* IRQ1 (keyboard) fires with interrupts disabled (it's an interrupt gate),
   and command processing (handle_keyboard_byte -> run_command) can end up
   calling something that needs interrupts ON to ever wake back up. So the
   ISR itself does the least possible: stash the raw scancode in a small
   ring buffer and return immediately. The real work happens in the main
   loop, outside interrupt context, where blocking is safe. */
#define KBD_BUF_SIZE 64
extern volatile uint8_t kbd_buffer[KBD_BUF_SIZE];
extern volatile int kbd_buf_head;
extern volatile int kbd_buf_tail;

void keyboard_irq_handler(registers_t* regs);
void handle_keyboard_byte(uint8_t scancode);

/* Longjmps back to the checkpoint handle_keyboard_byte takes right
   before running a command - called by paging.c's page fault handler
   when task 0's guard page (see boot.s) is hit, i.e. some command
   (very plausibly `run <script>.ernx` - see ernxscript.c) overflowed
   task 0's stack. The crashed command's own state is gone, but the
   shell loop itself resumes normally instead of the whole system
   halting. Never returns. */
void task0_recover(void) __attribute__((noreturn));

/* unshifted scancode->ASCII table, shared with ERNXscript's `input`
   statement (ernx_read_line_int), which reads raw keys the same way the
   shell's own line editor does. */
extern const char scancode_ascii[128];

#endif /* ERNXOS_KEYBOARD_H */

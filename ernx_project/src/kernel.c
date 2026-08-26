#include <stdint.h>

#include "vga.h"
#include "util.h"
#include "interrupts.h"
#include "keyboard.h"
#include "mouse.h"
#include "wm.h"
#include "vfs.h"
#include "disk.h"
#include "fs.h"
#include "hw.h"
#include "gfx.h"
#include "paging.h"

/* defined in boot.s - the unmapped page directly below task 0's own
   16 KiB boot stack (see the comment there, and paging.h/paging.c). */
extern uint8_t task0_guard_page[];

/* ---------------- kernel entry point ----------------
   Everything else in this kernel lives in its own module (see the other
   files under src/);
   this file only wires them together in boot order. `magic`/`info_addr`
   come from the multiboot bootloader (GRUB), passed in by boot.s. */

void kernel_main(uint32_t magic, uint32_t info_addr) {
    terminal_clear();
    terminal_writestring("My kernel is running!\n");
    terminal_writestring("Type 'help' for commands.\n> ");

    idt_init();
    /* Paging must come up before anything creates a task: task_create()
       plants a guard page below each task's stack, which needs the page
       tables to already exist. It also must come after idt_init(), so
       that a mistake anywhere in paging_init() itself would at least
       land on a real (if unhandled) page fault instead of a triple
       fault with no diagnostics at all. */
    paging_init();
    /* Guard task 0's own stack the same way task_create() guards every
       task after it - see boot.s and paging_guard_task0(). */
    paging_guard_task0((uint32_t) task0_guard_page);
    pit_init(PIT_HZ);
    task_init();
    task_create(heartbeat_task);

    multiboot_load_modules(magic, info_addr);

    disk_init();
    if (disk_ready) {
        terminal_writestring("Disk ready (");
        print_int(disk_file_count);
        terminal_writestring(" saved file(s), ");
        print_int((int) disk_next_free_sector);
        terminal_writestring("/");
        print_int((int) disk_total_sectors);
        terminal_writestring(" sectors used).\n> ");
    } else {
        terminal_writestring("No disk attached - files won't persist.\n> ");
    }

    mouse_init();
    mouse_show_position();

    /* Capture the BIOS/GRUB-loaded text-mode font before graphics mode
       ever runs, so it can be restored every time we come back to text
       mode (see vga_restore_text_font). Must happen before cmd_gfx()
       below - that's the first thing that touches Mode 13h. */
    vga_save_text_font();

    /* Start the first graphical desktop with two useful windows. The shell
       is still available with ESC, so this is a desktop-first boot rather
       than removing the command line. */
    window_count = 0;
    active_window_idx = 0;
    /* Keep the launch icons clear at the top of the desktop. */
    window_create(8, 3, 11, 40, "TERMINAL");
    window_create(8, 43, 11, 36, "FILES");
    if (window_count > 0) window_focus(0);

    irq_install_handler(1, keyboard_irq_handler);
    irq_install_handler(12, mouse_irq_handler);
    irq_clear_mask(2); /* cascade line - lets slave-PIC IRQs (8-15, incl. 12) reach the CPU */

    __asm__ volatile ("sti");

    /* Desktop-first boot: enter the same graphics environment that the
       `gfx` command provides. Press ESC inside the desktop to return to the
       normal shell. */
    cmd_gfx();

    /* everything now arrives via interrupts (PIT/keyboard/mouse). Mouse
       bytes are handled straight from the ISR; keyboard scancodes are
       drained here, outside interrupt context, so commands they trigger
       (e.g. beep's sleep_ms) can safely wait for later ticks. hlt sleeps
       the CPU until the next interrupt when there's nothing to drain. */
    while (1) {
        while (kbd_buf_tail != kbd_buf_head) {
            uint8_t scancode = kbd_buffer[kbd_buf_tail];
            kbd_buf_tail = (kbd_buf_tail + 1) % KBD_BUF_SIZE;
            handle_keyboard_byte(scancode);
        }
        __asm__ volatile ("hlt");
    }
}

#ifndef ERNXOS_INTERRUPTS_H
#define ERNXOS_INTERRUPTS_H

#include <stdint.h>

/* ---------------- IDT / PIC / real interrupts ----------------
 *
 * Everything used to be polling: the main loop asked the keyboard/mouse
 * controller "got anything for me?" in a tight spin. That wastes CPU and
 * doesn't scale to more devices. This section wires up real hardware
 * interrupts instead: the CPU calls straight into one of the isrN/irqN
 * stubs in idt_asm.s the instant a device raises a line, we ack it, and
 * dispatch to whichever C handler registered for that IRQ.
 *
 * The two PICs (8259s) default to firing IRQ0-15 on interrupt vectors
 * 0x08-0x0F, which collides with the CPU's own exception vectors
 * (0x00-0x1F). pic_remap() moves them out of the way to 0x20-0x2F first.
 */

typedef struct {
    uint32_t edi, esi, ebp, esp_orig, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*irq_handler_t)(registers_t*);

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

void idt_init(void);
void irq_install_handler(uint8_t irq_line, irq_handler_t handler);
void irq_clear_mask(uint8_t irq_line);

/* Called by the isrN/irqN assembly stubs in idt_asm.s. */
void isr_handler(registers_t* regs);
void irq_handler(registers_t* regs);

#endif /* ERNXOS_INTERRUPTS_H */

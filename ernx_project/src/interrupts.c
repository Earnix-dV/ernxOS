#include "interrupts.h"
#include "io.h"
#include "vga.h"
#include "util.h"
#include "paging.h"

struct idt_entry {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  always0;
    uint8_t  flags;
    uint16_t base_hi;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idt_flush(uint32_t idt_ptr_addr);

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static irq_handler_t irq_handlers[16];

/* remaps the PICs' IRQ0-15 onto interrupt vectors 32-47, and starts with
   every line masked off - callers opt in per-IRQ with irq_clear_mask()
   once they've actually installed a handler for it. */
static void pic_remap(void) {
    outb(PIC1_COMMAND, 0x11); /* begin init sequence, expect 3 more bytes (ICW1) */
    outb(PIC2_COMMAND, 0x11);
    outb(PIC1_DATA, 0x20);    /* ICW2: master IRQs start at vector 0x20 */
    outb(PIC2_DATA, 0x28);    /* ICW2: slave IRQs start at vector 0x28  */
    outb(PIC1_DATA, 0x04);    /* ICW3: slave PIC lives on master's IRQ2 */
    outb(PIC2_DATA, 0x02);    /* ICW3: slave tells master which line it's on */
    outb(PIC1_DATA, 0x01);    /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01);
    outb(PIC1_DATA, 0xFF);    /* mask everything until a handler asks for it */
    outb(PIC2_DATA, 0xFF);
}

void irq_clear_mask(uint8_t irq_line) {
    uint16_t port = irq_line < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq_line < 8 ? irq_line : (uint8_t) (irq_line - 8);
    outb(port, (uint8_t) (inb(port) & ~(1 << bit)));
}

void irq_install_handler(uint8_t irq_line, irq_handler_t handler) {
    irq_handlers[irq_line] = handler;
    irq_clear_mask(irq_line);
}

static void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = (uint16_t) (base & 0xFFFF);
    idt[num].base_hi = (uint16_t) ((base >> 16) & 0xFFFF);
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
}

/* CPU exceptions (divide-by-zero, page fault, ...) land here when nothing
   more specific handles them. There's no recovery story for most of
   these yet, so just stop cleanly instead of ploughing on with corrupted
   state. */
void isr_handler(registers_t* regs) {
    /* vector 14 = #PF (page fault) - the one exception we can actually
       do something about now that paging is on. Everything else below
       is still an unconditional halt: without per-task address spaces
       (ring 3 + a TSS, which this kernel doesn't have yet) there's no
       safe way to assume any other exception is contained to one task. */
    if (regs->int_no == 14) page_fault_handler(regs); /* never returns */

    terminal_writestring("\n*** unhandled CPU exception ");
    print_int((int) regs->int_no);
    terminal_writestring(" - halted ***\n");
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}

void irq_handler(registers_t* regs) {
    uint32_t irq_line = regs->int_no - 32;
    if (irq_line >= 8) outb(PIC2_COMMAND, PIC_EOI); /* ack slave first */
    outb(PIC1_COMMAND, PIC_EOI);                    /* then master */
    if (irq_line < 16 && irq_handlers[irq_line]) irq_handlers[irq_line](regs);
}

void idt_init(void) {
    idtp.limit = (uint16_t) (sizeof(idt) - 1);
    idtp.base = (uint32_t) &idt;
    for (int i = 0; i < 256; i++) idt_set_gate((uint8_t) i, 0, 0, 0);

    /* whatever code selector is active right now is the one GRUB set up
       as a flat 32-bit segment - reuse it rather than guessing 0x08. */
    uint16_t cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    void (*isr_stubs[32])(void) = {
        isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
        isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
        isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
        isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    };
    for (int i = 0; i < 32; i++) idt_set_gate((uint8_t) i, (uint32_t) isr_stubs[i], cs, 0x8E);

    void (*irq_stubs[16])(void) = {
        irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
        irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
    };
    for (int i = 0; i < 16; i++) idt_set_gate((uint8_t) (32 + i), (uint32_t) irq_stubs[i], cs, 0x8E);

    pic_remap();
    idt_flush((uint32_t) &idtp);
}

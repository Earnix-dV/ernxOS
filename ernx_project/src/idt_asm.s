/* idt_asm.s - low-level interrupt entry points.
 *
 * The CPU jumps straight to one of these on interrupt/exception; a C
 * function can't be an interrupt target directly because someone has to
 * push the missing error code (some exceptions push one, most don't),
 * save every register the C compiler might clobber, and IRET (not RET)
 * back out. Each stub does exactly that and then hands off to a single
 * C dispatcher (isr_handler for CPU exceptions 0-31, irq_handler for
 * hardware IRQs remapped to 32-47).
 *
 * We deliberately don't touch %ds/%es/%fs/%gs here: GRUB's multiboot
 * loader already leaves flat data segments loaded, this kernel never
 * builds its own GDT or switches privilege levels, so those selectors
 * stay valid on both sides of the interrupt with no reloading needed.
 */

.section .text

.macro ISR_NOERR num
.global isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    push $\num
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_NOERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* num: vector actually raised on the wire (32-47).
   IRQ0 -> vector 32, IRQ1 -> vector 33, ... IRQ15 -> vector 47
   (this remap happens in the PIC, C side just needs to know the vector). */
.macro IRQ_STUB irqnum, vector
.global irq\irqnum
irq\irqnum:
    push $0
    push $\vector
    jmp irq_common_stub
.endm

IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

.extern isr_handler
.extern irq_handler

isr_common_stub:
    pusha
    push %esp        /* pointer to the registers_t we just built */
    call isr_handler
    add $4, %esp
    popa
    add $8, %esp      /* drop err_code + int_no pushed above */
    iret

irq_common_stub:
    pusha
    push %esp
    call irq_handler
    add $4, %esp
    popa
    add $8, %esp
    iret

.global idt_flush
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

.section .note.GNU-stack,"",@progbits

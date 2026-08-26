/* task_asm.s - the one piece of a preemptive scheduler that can't be
 * written in C: swapping the CPU over to a different stack.
 *
 * context_switch(uint32_t* old_esp_slot, uint32_t new_esp) is called as
 * an ordinary C function (normally from schedule(), in kernel.c). It:
 *   1. Pushes the registers the C caller expects to survive a call
 *      (ebp/ebx/esi/edi - callee-saved by cdecl) plus EFLAGS.
 *   2. Saves the resulting %esp into *old_esp_slot - this is the
 *      outgoing task's whole saved context, in one pointer.
 *   3. Loads %esp = new_esp - the incoming task's saved context,
 *      written either by a previous call to this same function, or by
 *      task_create() building a fake one for a brand-new task.
 *   4. Pops the same five values back off (now the incoming task's
 *      values) and `ret`s - which jumps to whatever return address
 *      sits on top of the incoming task's stack.
 *
 * That's the whole trick: every task's stack, whenever it isn't
 * running, always has this exact five-value frame sitting on top of
 * it, so any task can resume any other task through the same code path
 * without either of them needing to know it happened.
 */

.section .text

.global context_switch
context_switch:
    push %ebp
    push %ebx
    push %esi
    push %edi
    pushf

    mov 24(%esp), %eax   /* old_esp_slot */
    mov %esp, (%eax)     /* *old_esp_slot = current esp */

    mov 28(%esp), %eax   /* new_esp */
    mov %eax, %esp        /* jump onto the incoming task's stack */

    popf
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret

.section .note.GNU-stack,"",@progbits

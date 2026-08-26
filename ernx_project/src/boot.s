/* boot.s - Multiboot header + entry point */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0x1BADB002
.set CHECKSUM, -(MAGIC + FLAGS)

.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM

.section .bss
.align 4096
/* Guard page for task 0's own boot stack (see paging.c/task0_recover in
   keyboard.c). task_create() (hw.c) gives tasks 1+ a guard page below
   their stack automatically, but task 0 never goes through task_create -
   it's whatever's already running when task_init() is called, using the
   stack set up right here. Left unmapped by paging_init(), so a stack
   overflow on *this* stack (e.g. a runaway `run <script>.ernx` - see
   ernxscript.c's ernxscript_run) faults here instead of corrupting
   whatever .data/.bss happens to sit below it. */
.global task0_guard_page
task0_guard_page:
.skip 4096
stack_bottom:
.skip 16384   /* 16 KiB stack */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
    mov $stack_top, %esp

    push %ebx    /* multiboot info structure pointer */
    push %eax    /* multiboot magic number           */
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
.size _start, . - _start

.section .note.GNU-stack,"",@progbits

#ifndef ERNXOS_PAGING_H
#define ERNXOS_PAGING_H

#include <stdint.h>
#include "interrupts.h"

/* ---------------- paging / memory protection ----------------
 *
 * Stage 1 of real memory protection. Before this, every byte of RAM was
 * just... RAM - any pointer bug in any task could scribble over the
 * kernel, another task's stack, or the page tables themselves, and the
 * first anyone heard about it was a corrupted screen or a random crash
 * three instructions later. Paging gives the CPU a way to say "this
 * address isn't allowed" and fault *immediately*, at the exact
 * instruction that misbehaved, instead of letting bad writes through.
 *
 * This does NOT yet give each task its own private address space (that
 * needs a GDT with ring 3 segments + a TSS, which this kernel doesn't
 * have - everything still runs at ring 0 in one shared flat mapping).
 * What it gives us right now:
 *
 *   1. A page fault is a normal, recoverable event instead of undefined
 *      behaviour - CR2 tells us exactly which address was touched.
 *   2. Guard pages: task_create() (hw.c) leaves one unmapped page right
 *      below every task's stack, and boot.s leaves the same kind of gap
 *      below task 0's own stack (registered via paging_guard_task0(),
 *      called once from kernel.c). A stack overflow - on a background
 *      task, or on task 0 running a shell command (very plausibly `run
 *      <script>.ernx` - ernxscript.c's interpreter recurses per level
 *      of nesting) - now hits that hole and raises #PF on the exact
 *      write that would otherwise have trashed whatever memory
 *      happened to sit there.
 *   3. page_fault_handler() (called from isr_handler in interrupts.c)
 *      can tell a guard-page hit apart from every other fault, and
 *      tell task 0's guard page apart from a background task's. A
 *      background task gets killed outright (task_kill_current(),
 *      hw.c) and the scheduler moves on. Task 0 isn't a background
 *      task - it's the shell's own command loop - so there's nothing
 *      to "move on" to; instead it recovers back to right before the
 *      crashed command ran (task0_recover(), keyboard.c) and the shell
 *      itself keeps going. Every other fault (bad kernel pointer, etc.)
 *      still halts, because without ring 3 there's no safe way to
 *      assume it's isolated to one place.
 *
 * The identity map covers the first IDENTITY_MAP_MB of physical RAM as
 * 1:1 virtual==physical, read/write, supervisor-only - it doesn't
 * change any existing address in the kernel, it just makes the CPU
 * start checking them.
 */

#define PAGE_SIZE       4096u
#define IDENTITY_MAP_MB 32u   /* matches run.sh's VBoxManage --memory 32 */

/* Builds the page directory/tables, identity-maps the first
   IDENTITY_MAP_MB of RAM, and turns paging on (sets CR3, then CR0.PG).
   Must run after idt_init() (so a fault mid-setup would at least print
   something) and before anything calls paging_guard_page(). */
void paging_init(void);

/* Unmaps the 4 KiB page containing `addr`, inside the identity-mapped
   range only. Any later access to that page raises #PF instead of
   silently reading/writing physical memory. Used by task_create() to
   plant a guard page below each task's stack. */
void paging_guard_page(uint32_t addr);

/* Same as paging_guard_page(), but also remembers `addr` as task 0's
   guard page specifically, so page_fault_handler() can tell "task 0's
   own stack overflowed" (recover via task0_recover(), keyboard.c) apart
   from every other fault (halt). Called once, in kernel.c, with
   boot.s's task0_guard_page symbol. */
void paging_guard_task0(uint32_t addr);

/* The C side of the #PF (vector 14) exception, called from
   isr_handler() in interrupts.c. Reads CR2 for the faulting address,
   decides whether it's a task's guard page (recoverable - kill just
   that task) or anything else (not safely recoverable without ring 3 -
   halt). Never returns: either it hands off to another task via
   task_kill_current(), or it halts. */
void page_fault_handler(registers_t* regs) __attribute__((noreturn));

#endif /* ERNXOS_PAGING_H */

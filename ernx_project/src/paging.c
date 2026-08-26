#include "paging.h"
#include "vga.h"
#include "util.h"
#include "hw.h"
#include "keyboard.h"

#define ENTRIES     1024u
#define TABLE_SPAN  (ENTRIES * PAGE_SIZE)                         /* 4 MiB per page table */
#define NUM_TABLES  ((IDENTITY_MAP_MB * 1024u * 1024u) / TABLE_SPAN) /* 32MB / 4MB = 8 */

/* Page directory and its page tables. Both must be page-aligned - the
   low 12 bits of every directory/table entry are flag bits, not part of
   the address, so the CPU (and we) require the tables themselves to
   start on a 4 KiB boundary. */
static uint32_t page_directory[ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static uint32_t page_tables[NUM_TABLES][ENTRIES] __attribute__((aligned(PAGE_SIZE)));

#define PTE_PRESENT  0x1u
#define PTE_WRITABLE 0x2u

static void identity_map(void) {
    for (uint32_t t = 0; t < NUM_TABLES; t++) {
        for (uint32_t i = 0; i < ENTRIES; i++) {
            uint32_t phys = t * TABLE_SPAN + i * PAGE_SIZE;
            page_tables[t][i] = phys | PTE_PRESENT | PTE_WRITABLE;
        }
        page_directory[t] = ((uint32_t) &page_tables[t][0]) | PTE_PRESENT | PTE_WRITABLE;
    }
    /* Everything past the identity-mapped range is simply not present -
       any access out there faults too, which is the correct behaviour
       (this kernel never allocates anything above IDENTITY_MAP_MB). */
    for (uint32_t t = NUM_TABLES; t < ENTRIES; t++) page_directory[t] = 0;
}

void paging_init(void) {
    identity_map();

    uint32_t pd_phys = (uint32_t) page_directory;
    __asm__ volatile (
        "mov %0, %%cr3\n"          /* point the CPU at our page directory */
        "mov %%cr0, %%eax\n"
        "or  $0x80000000, %%eax\n" /* CR0.PG (bit 31) - turn paging on */
        "mov %%eax, %%cr0\n"
        :
        : "r"(pd_phys)
        : "eax", "memory"
    );
}

void paging_guard_page(uint32_t addr) {
    uint32_t pd_idx = addr >> 22;
    uint32_t pt_idx = (addr >> 12) & 0x3FFu;
    if (pd_idx >= NUM_TABLES) return; /* outside the identity-mapped range - nothing to do */
    page_tables[pd_idx][pt_idx] = 0;  /* clear PTE_PRESENT: any access now raises #PF */
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory"); /* flush this address from the TLB */
}

/* task 0's guard page, remembered so page_fault_handler() can recognize
   it - see paging_guard_task0(). 0 is never a valid guard address (it's
   the very bottom of the identity-mapped range, always the multiboot/
   real-mode area, never a stack), so it doubles as "not yet registered". */
static uint32_t task0_guard_addr = 0;

void paging_guard_task0(uint32_t addr) {
    task0_guard_addr = addr;
    paging_guard_page(addr);
}

/* err_code layout for a #PF (Intel SDM Vol 3, 4.7):
   bit 0 = 1 if this was a protection violation (mapped but disallowed),
           0 if the page just wasn't present at all (our guard pages).
   bit 1 = 1 if the faulting access was a write, 0 if a read.
   bit 2 = 1 if it happened in user mode, 0 in supervisor mode (always 0
           here - this kernel has no ring 3 code yet). */
static uint32_t read_cr2(void) {
    uint32_t addr;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(addr));
    return addr;
}

void page_fault_handler(registers_t* regs) {
    uint32_t fault_addr = read_cr2();
    int was_write = (regs->err_code & 0x2) != 0;

    if (task0_guard_addr && fault_addr >= task0_guard_addr && fault_addr < task0_guard_addr + PAGE_SIZE) {
        terminal_writestring("\n*** stack overflow running that command - recovering ***\n");
        task0_recover(); /* never returns - longjmps back into keyboard.c's command loop */
    }

    int guard_owner = task_guard_page_owner(fault_addr);
    if (guard_owner >= 0) {
        terminal_writestring("\n*** task ");
        print_int(guard_owner);
        terminal_writestring(" stack overflow at 0x");
        print_hex(fault_addr);
        terminal_writestring(" - task killed, system continues ***\n> ");
        task_kill_current(); /* never returns */
    }

    terminal_writestring("\n*** page fault (");
    terminal_writestring(was_write ? "write" : "read");
    terminal_writestring(") at 0x");
    print_hex(fault_addr);
    terminal_writestring(" - halted ***\n");
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}

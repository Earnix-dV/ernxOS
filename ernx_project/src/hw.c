#include "hw.h"
#include "io.h"
#include "vga.h"
#include "util.h"
#include "interrupts.h"
#include "paging.h"

/* ---------------- hardware: PC speaker, RTC clock, reboot ---------------- */

#define PIT_CHANNEL2_PORT 0x42
#define PIT_COMMAND_PORT  0x43
#define PIT_FREQUENCY     1193182u
#define SPEAKER_PORT      0x61

/* programs PIT channel 2 to the given frequency and gates it into the PC
   speaker. The speaker keeps tone-ing until pc_speaker_off() is called. */
static void pc_speaker_on(uint32_t freq_hz) {
    if (freq_hz == 0) freq_hz = 1;
    if (freq_hz > PIT_FREQUENCY) freq_hz = PIT_FREQUENCY;
    uint32_t divisor = PIT_FREQUENCY / freq_hz;
    if (divisor == 0) divisor = 1;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    outb(PIT_COMMAND_PORT, 0xB6); /* channel 2, lobyte/hibyte access, square wave */
    outb(PIT_CHANNEL2_PORT, (uint8_t) (divisor & 0xFF));
    outb(PIT_CHANNEL2_PORT, (uint8_t) ((divisor >> 8) & 0xFF));
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp | 0x03); /* bit0: gate PIT channel 2, bit1: speaker data */
}

static void pc_speaker_off(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}

/* ---------------- preemptive tasks ----------------
 *
 * A handful of statically-allocated tasks, switched round-robin. Each
 * task gets its own small stack; "switching" just means saving the
 * current stack pointer and loading a different one (context_switch,
 * in task_asm.s) - the CPU registers, and where execution resumes,
 * live entirely on that stack, so restoring it restores the task.
 *
 * The scheduler is driven straight from the timer tick (see
 * pit_irq_handler below): every tick hands the CPU to the next task in
 * line, whether or not the current one asked for that. That's what
 * makes it preemptive rather than cooperative - a task that's stuck in
 * a long wait (like beep's sleep_ms(), which just hlts until enough
 * ticks pass) still gets interrupted on schedule, so it doesn't freeze
 * everything else out. Task 0 is whichever task is already running
 * when task_init() is called - normally the shell's own kernel_main
 * loop - captured the first time it gets switched away from; nothing
 * needs to be done for it up front.
 */

#define MAX_TASKS         4
#define TASK_STACK_PAGES  4  /* 16 KiB usable stack per task */

/* Each task's region is [ guard page | TASK_STACK_PAGES stack pages ],
   page-aligned so paging_guard_page() can unmap exactly the first page
   of it. That leading page is left unmapped once paging comes up (see
   task_create below), so a stack overflow - the classic way one task
   corrupts memory that isn't its own - hits that hole and raises a
   page fault on the exact instruction that would otherwise have
   scribbled over whatever used to sit there, instead of silently
   trashing it. task 0 (whatever's running when task_init() is called,
   normally kernel_main's own loop) keeps using boot.s's stack and isn't
   guarded this way yet - see paging.h for what's still missing. */
#define TASK_REGION_PAGES (TASK_STACK_PAGES + 1)
#define TASK_REGION_BYTES (TASK_REGION_PAGES * PAGE_SIZE)

static uint8_t task_region[MAX_TASKS][TASK_REGION_BYTES] __attribute__((aligned(PAGE_SIZE)));

typedef struct {
    uint32_t esp;
    int      alive;
} task_t;

static task_t tasks[MAX_TASKS];
static int num_tasks = 0;
static int current_task_idx = 0;

extern void context_switch(uint32_t* old_esp_slot, uint32_t new_esp);

/* registers a new task and gives it a stack pre-built to look exactly
   like a task that's already mid-context_switch, waiting to be resumed
   for the first time - so the normal switch path in schedule() can
   start it without any special-casing. `entry` must never return (make
   it an infinite loop) since there's nothing for it to return into. */
int task_create(void (*entry)(void)) {
    if (num_tasks >= MAX_TASKS) return -1;
    int idx = num_tasks;

    uint8_t* region = task_region[idx];
    paging_guard_page((uint32_t) region); /* unmap page 0 of the region - the guard page */

    uint32_t* sp = (uint32_t*) (region + TASK_REGION_BYTES); /* top of the stack pages */
    *(--sp) = (uint32_t) entry; /* context_switch's `ret` lands here */
    *(--sp) = 0;                /* ebp */
    *(--sp) = 0;                /* ebx */
    *(--sp) = 0;                /* esi */
    *(--sp) = 0;                /* edi */
    *(--sp) = 0x202;            /* eflags, IF set so the new task runs with interrupts on */

    tasks[idx].esp = (uint32_t) sp;
    tasks[idx].alive = 1;

    return num_tasks++;
}

void task_init(void) {
    num_tasks = 1;       /* slot 0 = whatever task is running right now */
    current_task_idx = 0;
    tasks[0].alive = 1;
}

/* Finds a live task other than `exclude`, searching round-robin from
   just after it. Returns -1 if there isn't one. Shared by schedule()
   (normal preemption - `exclude` is just where to start looking from)
   and task_kill_current() (the outgoing task is dead, not just paused). */
static int find_next_alive(int exclude) {
    for (int i = 1; i <= num_tasks; i++) {
        int idx = (exclude + i) % num_tasks;
        if (idx != exclude && tasks[idx].alive) return idx;
    }
    return -1;
}

/* round-robins to the next task, if there is one. Called from the timer
   ISR on every tick, so this can (and normally does) run while the
   current task is mid-instruction somewhere completely unrelated -
   that's fine, context_switch just freezes it exactly there. */
static void schedule(void) {
    if (num_tasks <= 1) return; /* nobody else to run */
    int prev_idx = current_task_idx;
    int next_idx = find_next_alive(prev_idx);
    if (next_idx < 0) return; /* everyone else is dead */
    current_task_idx = next_idx;
    context_switch(&tasks[prev_idx].esp, tasks[next_idx].esp);
}

/* fault_addr falls in task i's guard page iff it lands in the first
   PAGE_SIZE bytes of task_region[i] - only defined tasks (index <
   num_tasks) have a region worth checking. */
int task_guard_page_owner(uint32_t fault_addr) {
    for (int i = 1; i < num_tasks; i++) { /* task 0 has no guarded region (see above) */
        uint32_t guard_start = (uint32_t) task_region[i];
        if (fault_addr >= guard_start && fault_addr < guard_start + PAGE_SIZE) return i;
    }
    return -1;
}

void task_kill_current(void) {
    int dead = current_task_idx;
    tasks[dead].alive = 0;

    int next = find_next_alive(dead);
    if (next < 0) {
        terminal_writestring("*** no tasks left to run - halted ***\n");
        __asm__ volatile ("cli");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    current_task_idx = next;
    uint32_t discard_esp; /* the dead task's context is never coming back - nothing to save it into */
    context_switch(&discard_esp, tasks[next].esp);
    __builtin_unreachable();
}

/* PIT channel 0, driven through IRQ0, as a real tick counter. Every tick
   is one PIT_HZ'th of a second, so sleep_ms() can wait for an exact
   number of ticks instead of guessing at a loop count that only happens
   to "feel right" on one particular machine or VM. uptime tracks whole
   seconds the same way, counted as ticks come in rather than re-reading
   the CMOS RTC by hand every time someone asks. */
#define PIT_HZ 100u

volatile uint32_t pit_ticks = 0;

static void pit_irq_handler(registers_t* regs) {
    (void) regs;
    pit_ticks++;
    schedule();
}

void pit_init(uint32_t freq_hz) {
    uint32_t divisor = PIT_FREQUENCY / freq_hz;
    outb(PIT_COMMAND_PORT, 0x36); /* channel 0, lobyte/hibyte access, mode 3 square wave */
    outb(0x40, (uint8_t) (divisor & 0xFF));
    outb(0x40, (uint8_t) ((divisor >> 8) & 0xFF));
    irq_install_handler(0, pit_irq_handler);
}

/* real, calibrated delay: waits for pit_ticks to actually advance far
   enough, sleeping (hlt) between ticks instead of burning CPU. Needs
   interrupts enabled - if they're off this would hang forever, so bail
   out and fall back to not waiting at all rather than lock up the OS. */
void sleep_ms(uint32_t ms) {
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    if (!(eflags & 0x200)) return; /* interrupts disabled - can't safely wait */

    uint32_t ticks_needed = (ms * PIT_HZ) / 1000;
    if (ticks_needed == 0) ticks_needed = 1;
    uint32_t target = pit_ticks + ticks_needed;
    while (pit_ticks < target) {
        __asm__ volatile ("hlt");
    }
}

/* demo background task: spins a little |/-\ marker in the bottom-right
   corner, twice a second, forever. It runs on its own stack via the
   scheduler above and never touches the shell's state - the point of
   it is purely to stay visibly moving even while the shell task is
   sitting inside a blocking sleep_ms() (e.g. running `beep`), proving
   the system as a whole isn't frozen just because one task is waiting. */
void heartbeat_task(void) {
    static const char frames[4] = { '|', '/', '-', '\\' };
    int i = 0;
    for (;;) {
        VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + (VGA_WIDTH - 1)] = vga_entry(frames[i], 0x0A);
        i = (i + 1) % 4;
        sleep_ms(250);
    }
}

/* uptime - seconds since boot, from the tick counter rather than the RTC. */
void cmd_uptime(void) {
    uint32_t total_seconds = pit_ticks / PIT_HZ;
    uint32_t hours = total_seconds / 3600;
    uint32_t minutes = (total_seconds % 3600) / 60;
    uint32_t seconds = total_seconds % 60;

    terminal_writestring("Uptime: ");
    print_int((int) hours);
    terminal_writestring("h ");
    print_int((int) minutes);
    terminal_writestring("m ");
    print_int((int) seconds);
    terminal_writestring("s\n");
}

/* beep [freq_hz] - plays a tone through the PC speaker for a short, fixed
   duration. Defaults to 1000 Hz if no frequency (or a non-numeric one) is
   given. */
void cmd_beep(char* args) {
    uint32_t freq = 1000;
    if (args[0] != '\0') {
        uint32_t v = 0;
        int valid = 1;
        for (int i = 0; args[i] != '\0'; i++) {
            if (args[i] < '0' || args[i] > '9') { valid = 0; break; }
            v = v * 10 + (uint32_t) (args[i] - '0');
        }
        if (valid && v > 0) freq = v;
    }

    terminal_writestring("Beep at ");
    print_int((int) freq);
    terminal_writestring(" Hz...\n");

    pc_speaker_on(freq);
    sleep_ms(300);
    pc_speaker_off();
}

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

/* the RTC updates its registers roughly once a second; while it does, a read
   can land mid-update and come back torn. Register A's bit 7 flags that, so
   callers wait for it to clear before reading the time fields. */
static int cmos_update_in_progress(void) {
    outb(CMOS_ADDRESS, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t) ((val & 0x0F) + ((val / 16) * 10));
}

/* time - reads and prints the current date/time from the CMOS real-time
   clock. Handles both the BCD and binary encodings, and 12-hour/PM, since
   which one a given machine (or VM) uses depends on register B. */
void cmd_time(void) {
    uint32_t guard = 0;
    while (cmos_update_in_progress() && guard < 1000000) guard++;

    uint8_t second = cmos_read(0x00);
    uint8_t minute = cmos_read(0x02);
    uint8_t hour   = cmos_read(0x04);
    uint8_t day    = cmos_read(0x07);
    uint8_t month  = cmos_read(0x08);
    uint8_t year   = cmos_read(0x09);
    uint8_t regB   = cmos_read(0x0B);

    if (!(regB & 0x04)) { /* not "always binary" - values are BCD, convert */
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour   = (uint8_t) (bcd_to_bin(hour & 0x7F) | (hour & 0x80));
        day    = bcd_to_bin(day);
        month  = bcd_to_bin(month);
        year   = bcd_to_bin(year);
    }
    if (!(regB & 0x02) && (hour & 0x80)) { /* 12-hour mode with the PM bit set */
        hour = (uint8_t) (((hour & 0x7F) + 12) % 24);
    }

    terminal_writestring("Date: 20");
    if (year < 10) terminal_putchar('0');
    print_int((int) year);
    terminal_putchar('-');
    if (month < 10) terminal_putchar('0');
    print_int((int) month);
    terminal_putchar('-');
    if (day < 10) terminal_putchar('0');
    print_int((int) day);
    terminal_writestring("  Time: ");
    if (hour < 10) terminal_putchar('0');
    print_int((int) hour);
    terminal_putchar(':');
    if (minute < 10) terminal_putchar('0');
    print_int((int) minute);
    terminal_putchar(':');
    if (second < 10) terminal_putchar('0');
    print_int((int) second);
    terminal_putchar('\n');
}

/* reboot - pulses the CPU reset line through the 8042 keyboard controller
   (the same trick real BIOSes use for a software reset). If the controller
   never takes the command (some emulators ignore it), hang rather than
   falling through into whatever comes after in memory. */
void cmd_reboot(void) {
    terminal_writestring("Rebooting...\n");

    uint32_t guard = 0;
    while ((inb(0x64) & 0x02) != 0 && guard < 1000000) guard++; /* wait: input buffer empty */
    outb(0x64, 0xFE);

    while (1) { }
}


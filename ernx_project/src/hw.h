#ifndef ERNXOS_HW_H
#define ERNXOS_HW_H

#include <stdint.h>

/* ---------------- hardware: PIT, PC speaker, preemptive tasks, RTC, reboot ---------------- */

#define PIT_HZ 100u

/* real tick counter, advanced by the PIT IRQ handler - used as a PRNG
   seed by ERNXscript's `random`, in addition to sleep_ms/cmd_uptime here. */
extern volatile uint32_t pit_ticks;

void pit_init(uint32_t freq_hz);

void task_init(void);
int task_create(void (*entry)(void));
void heartbeat_task(void);

/* Returns the index of the task whose guard page `fault_addr` falls in,
   or -1 if it doesn't belong to any task's guard page. Used by
   paging.c's page fault handler to tell "task overflowed its stack"
   (recoverable) apart from every other fault (not). */
int task_guard_page_owner(uint32_t fault_addr);

/* Marks the currently running task dead and switches straight to the
   next alive one - called from the page fault handler once it decides
   the current task can't be safely resumed. Never returns: if no other
   task is alive, it halts instead (there's nothing left to fall back
   to). */
void task_kill_current(void) __attribute__((noreturn));

void sleep_ms(uint32_t ms);

/* shell commands */
void cmd_uptime(void);
void cmd_beep(char* args);
void cmd_time(void);
void cmd_reboot(void);

#endif /* ERNXOS_HW_H */

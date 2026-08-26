#include "keyboard.h"
#include "io.h"
#include "vga.h"
#include "wm.h"
#include "shell.h"

/* keyboard data lives on the same PS/2 controller data port (0x60) as the
   mouse - this constant is kept local to the keyboard driver rather than
   shared, since both drivers only ever read/write this one port. */
#define KBD_DATA_PORT 0x60

const char scancode_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0,' ',
};

static const char scancode_ascii_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0,' ',
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LSHIFT_REL 0xAA
#define SC_RSHIFT_REL 0xB6
#define SC_ALT 0x38
#define SC_ALT_REL 0xB8
#define SC_TAB 0x0F

static int shift_held = 0;
static int alt_held = 0;

/* input line buffer for simple commands */
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len = 0;

/* ---------------- task 0 crash recovery ----------------
 *
 * run_command() (called below) executes entirely on task 0's own stack,
 * synchronously, for as long as the command takes - including `run
 * <script>.ernx`, which recurses through ernxscript.c's expression
 * evaluator for as many nested parens/blocks as the script has. A
 * deeply-nested or buggy script can still overflow task 0's stack the
 * same way it always could; the guard page below that stack (see
 * boot.s) turns that overflow into a page fault instead of silent
 * corruption, but a fault still has to land *somewhere* survivable.
 * This is that somewhere: checkpoint right before running a command,
 * and if task0_recover() gets called (from paging.c's page fault
 * handler, once it recognizes the fault as this exact guard page),
 * jump straight back here instead of letting the fault propagate into
 * a halt. This is the one thing tasks 1+ *don't* need - they just get
 * killed and the scheduler moves on (see task_kill_current, hw.c) -
 * because task 0 isn't a background task, it's this loop; there's
 * nothing to "move on" to. */
static void* task0_jmpbuf[5];

void task0_recover(void) {
    __builtin_longjmp(task0_jmpbuf, 1);
}

void handle_keyboard_byte(uint8_t scancode) {
    if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) { shift_held = 1; return; }
    if (scancode == SC_LSHIFT_REL || scancode == SC_RSHIFT_REL) { shift_held = 0; return; }
    if (scancode == SC_ALT) { alt_held = 1; return; }
    if (scancode == SC_ALT_REL) { alt_held = 0; return; }
    if (scancode & 0x80) return; /* other key releases, ignore */
    if (scancode >= 128) return; /* extended-key prefix / unsupported scancode */

    /* Alt+Tab: switch to next window */
    if (alt_held && scancode == SC_TAB) {
        if (window_count > 1) {
            int next = (active_window_idx + 1) % window_count;
            window_focus(next);
            terminal_clear();
            window_redraw_all();
            terminal_writestring("Switched to window: ");
            terminal_writestring(windows[next].title);
            terminal_putchar('\n');
            terminal_writestring("> ");
        }
        return;
    }

    char c = shift_held ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
    if (!c) return;

    if (c == '\n') {
        cmd_buf[cmd_len] = '\0';
        if (__builtin_setjmp(task0_jmpbuf) == 0) {
            run_command(cmd_buf);
        } else {
            terminal_writestring("[recovered - that command overflowed the stack]\n> ");
        }
        cmd_len = 0;
    } else if (c == '\b') {
        if (cmd_len > 0) {
            cmd_len--;
            terminal_putchar('\b');
        }
    } else {
        if (cmd_len < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_len++] = c;
            terminal_putchar(c);
        }
    }
}

volatile uint8_t kbd_buffer[KBD_BUF_SIZE];
volatile int kbd_buf_head = 0;
volatile int kbd_buf_tail = 0;

void keyboard_irq_handler(registers_t* regs) {
    (void) regs;
    uint8_t data = inb(KBD_DATA_PORT);
    int next = (kbd_buf_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_buf_tail) { /* buffer full: drop the byte rather than block here */
        kbd_buffer[kbd_buf_head] = data;
        kbd_buf_head = next;
    }
}

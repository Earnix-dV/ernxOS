# v4: split into modules

The kernel used to be a single ~3800-line `src/kernel.c`. It's now split into
one `.c`/`.h` pair per subsystem, each compiled to its own object and linked
together (see `build.sh`). No logic was changed - every function body is
byte-for-byte the same as before, just moved and given an `extern` where
another module now needs to call it. `kernel.c` itself is now just the ~80
line `kernel_main()` that wires everything together at boot.

| Module          | Contents                                                        |
|-----------------|-------------------------------------------------------------------|
| `io.h`          | `inb`/`outb`/`inw`/`outw` port I/O primitives                     |
| `vga.c/.h`      | VGA text terminal (scrolling console) + hardware cursor           |
| `util.c/.h`     | string/byte helpers used everywhere (no libc available)           |
| `interrupts.c/.h` | IDT, PIC remap/mask, ISR/IRQ dispatch                           |
| `keyboard.c/.h` | PS/2 keyboard driver, scancode tables, IRQ1 handler                |
| `mouse.c/.h`    | PS/2 mouse driver, IRQ12 handler                                   |
| `wm.c/.h`       | text-mode window manager + window shell commands                   |
| `vfs.c/.h`      | in-RAM file table, path resolution, initrd loading                 |
| `disk.c/.h`     | ATA PIO disk driver                                                 |
| `fs.c/.h`       | persistent on-disk filesystem + file shell commands (ls/cat/...)   |
| `hw.c/.h`       | PIT, PC speaker, preemptive task scheduler, RTC, reboot             |
| `gfx.c/.h`      | VGA Mode 13h graphics + the graphical desktop (`gfx` command)       |
| `ernxscript.c/.h` | the `.ernx` scripting language interpreter                       |
| `shell.c/.h`    | command-line dispatcher (`run_command`)                             |
| `kernel.c`      | `kernel_main` - boot sequence only                                  |

## Bug check

Compiled the original single-file kernel with `-Wall -Wextra` before starting:
only one warning came up, `gfx_draw_line` defined-but-unused (dead code, a
Bresenham line primitive nothing called yet). Fixed by exporting it as a
public `gfx.h` utility (same treatment the original author already gave a
similar case, `gfx_focus_window_at`, via `__attribute__((unused))`) instead of
deleting it. No other bugs found - command buffer, window array, ERNXscript
variable table, and disk sector math all already had correct bounds checks.

## Verified

- Every module compiles cleanly with `-Wall -Wextra`, zero warnings.
- Full `build.sh` run (assemble -> compile each module -> link -> pack
  initrd) succeeds and produces a kernel binary with the same multiboot
  entry point and all expected strings/behavior intact.
- `grub-mkrescue`/QEMU aren't available in the sandbox this was built in, so
  the final ISO step and an actual boot-to-desktop run couldn't be tested
  here - worth a real boot test on your end before relying on it.
## v5: CALC app

Added a real integer calculator as a fourth desktop app, alongside FILES,
TERMINAL, and GAMES - same window system, no new plumbing needed.

- Desktop icon (click it, or press **C** on the keyboard while the desktop
  has focus) opens a `CALC` window: a small screen at the top, a 4x4 keypad
  below it (`7 8 9 /`, `4 5 6 *`, `1 2 3 -`, `C 0 = +`).
- Chained input works normally (`5 + 3 + 2 =` → 8, evaluating left to
  right as you go, same as a pocket calculator).
- `C` clears everything. Dividing by zero shows `Err` instead of crashing;
  pressing any digit after that starts a fresh number.
- Only integer math - this kernel has no floating-point support at all (no
  libc, no soft-float), so `7 / 2` shows `3`, not `3.5`, same truncation
  C's own `/` does. `*` and `/` stand in for ×/÷: the built-in 8x8 font only
  covers what ERNXscript already used (plus A-Z/0-9/punctuation), so there's
  no ÷/× glyph to draw.
- New generic helpers `int_to_str`/`str_to_int` went into `util.c` (turning
  a number into a string, and back) - useful beyond the calculator, so they
  live with the other string helpers rather than inside `gfx.c`.
- Everything else (button layout, click hit-testing, calculator state) is
  self-contained in `gfx.c`, following the same pattern FILES/GAMES already
  use for their own click routing.


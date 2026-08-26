#include "ernxscript.h"
#include <stdint.h>
#include "vga.h"
#include "util.h"
#include "vfs.h"
#include "keyboard.h"
#include "shell.h"
#include "vfs.h"
#include "hw.h"

/* ============= ERNXscript Interpreter Types ============= */

typedef enum {
    ERNX_TOK_EOF, ERNX_TOK_NUMBER, ERNX_TOK_STRING, ERNX_TOK_IDENT,
    ERNX_TOK_VAR, ERNX_TOK_IF, ERNX_TOK_WHILE, ERNX_TOK_FUNC, ERNX_TOK_END,
    ERNX_TOK_RETURN, ERNX_TOK_PRINT, ERNX_TOK_SHELL, ERNX_TOK_LPAREN, ERNX_TOK_RPAREN,
    ERNX_TOK_ASSIGN, ERNX_TOK_EQ, ERNX_TOK_NE, ERNX_TOK_LT, ERNX_TOK_GT, ERNX_TOK_LE, ERNX_TOK_GE,
    ERNX_TOK_PLUS, ERNX_TOK_MINUS, ERNX_TOK_MUL, ERNX_TOK_DIV, ERNX_TOK_MOD,
    ERNX_TOK_AND, ERNX_TOK_OR, ERNX_TOK_NOT, ERNX_TOK_COMMA,
    ERNX_TOK_BREAK, ERNX_TOK_RANDOM, ERNX_TOK_INPUT,
} ernx_token_type_t;

typedef struct {
    ernx_token_type_t type;
    union {
        int32_t num;
        char str[256];
    } val;
} ernx_token_t;

typedef struct {
    ernx_token_t* tokens;
    int count;
    int idx;
} ernx_parser_t;

typedef struct {
    char var_names[64][32];
    int32_t var_values[64];
    int num_vars;
} ernx_context_t;

static ernx_context_t g_ernx_ctx;

/* set by a `break` statement, checked by ernx_eval_block to unwind out of
   the statements still queued in the current block, and by the WHILE
   handler in ernx_eval_stmt to stop iterating. Cleared whenever a WHILE
   consumes it, so break only ever escapes the nearest enclosing loop. */
static int g_ernx_break = 0;

/* simple xorshift32 PRNG for the `random` expression - no libc, no HW
   RNG available, so this is seeded from the PIT tick counter (pit_ticks,
   defined further below) the first time it's used and then advances on
   every call, so back-to-back calls don't repeat. */
static uint32_t g_ernx_rand_state = 0;

/* ============= End ERNXscript Types ============= */

static inline int ernx_isdigit(int c) { return c >= '0' && c <= '9'; }
static inline int ernx_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int ernx_isalnum(int c) { return ernx_isalpha(c) || ernx_isdigit(c); }

/* Forward declarations for ERNXscript evaluator functions */
static int32_t ernx_eval_expr(ernx_parser_t* p);
static void ernx_eval_stmt(ernx_parser_t* p);

/* ============= End Forward Declarations ============= */

/* ============= ERNXscript Interpreter Implementation ============= */

static void ernx_tokenize(const char* source, ernx_token_t* tokens, int* count) {
    const char* pos = source;
    while (*pos && *count < 255) {
        while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;
        if (!*pos) break;
        if (*pos == '#') { while (*pos && *pos != '\n') pos++; continue; }
        
        ernx_token_t t;
        if (ernx_isdigit(*pos)) {
            t.type = ERNX_TOK_NUMBER;
            t.val.num = 0;
            while (*pos && ernx_isdigit(*pos)) t.val.num = t.val.num * 10 + (*pos++ - '0');
            tokens[(*count)++] = t;
        } else if (*pos == '"') {
            t.type = ERNX_TOK_STRING;
            pos++;
            int i = 0;
            while (*pos && *pos != '"' && i < 255) t.val.str[i++] = *pos++;
            t.val.str[i] = '\0';
            if (*pos == '"') pos++;
            tokens[(*count)++] = t;
        } else if (ernx_isalpha(*pos) || *pos == '_') {
            int i = 0;
            while (*pos && (ernx_isalnum(*pos) || *pos == '_') && i < 31) t.val.str[i++] = *pos++;
            t.val.str[i] = '\0';
            
            if (str_eq(t.val.str, "var")) t.type = ERNX_TOK_VAR;
            else if (str_eq(t.val.str, "if")) t.type = ERNX_TOK_IF;
            else if (str_eq(t.val.str, "while")) t.type = ERNX_TOK_WHILE;
            else if (str_eq(t.val.str, "end")) t.type = ERNX_TOK_END;
            else if (str_eq(t.val.str, "print")) t.type = ERNX_TOK_PRINT;
            else if (str_eq(t.val.str, "shell")) t.type = ERNX_TOK_SHELL;
            else if (str_eq(t.val.str, "and")) t.type = ERNX_TOK_AND;
            else if (str_eq(t.val.str, "or")) t.type = ERNX_TOK_OR;
            else if (str_eq(t.val.str, "not")) t.type = ERNX_TOK_NOT;
            else if (str_eq(t.val.str, "break")) t.type = ERNX_TOK_BREAK;
            else if (str_eq(t.val.str, "random")) t.type = ERNX_TOK_RANDOM;
            else if (str_eq(t.val.str, "input")) t.type = ERNX_TOK_INPUT;
            else t.type = ERNX_TOK_IDENT;
            tokens[(*count)++] = t;
        } else if (*pos == '=' && *(pos + 1) == '=') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_EQ }; pos += 2;
        } else if (*pos == '!' && *(pos + 1) == '=') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_NE }; pos += 2;
        } else if (*pos == '<' && *(pos + 1) == '=') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_LE }; pos += 2;
        } else if (*pos == '>' && *(pos + 1) == '=') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_GE }; pos += 2;
        } else if (*pos == '=') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_ASSIGN }; pos++;
        } else if (*pos == '<') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_LT }; pos++;
        } else if (*pos == '>') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_GT }; pos++;
        } else if (*pos == '+') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_PLUS }; pos++;
        } else if (*pos == '-') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_MINUS }; pos++;
        } else if (*pos == '*') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_MUL }; pos++;
        } else if (*pos == '/') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_DIV }; pos++;
        } else if (*pos == '%') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_MOD }; pos++;
        } else if (*pos == '(') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_LPAREN }; pos++;
        } else if (*pos == ')') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_RPAREN }; pos++;
        } else if (*pos == ',') { tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_COMMA }; pos++;
        } else { pos++; }
    }
    /* Reserve one slot for EOF so tokenization can never write past the
       fixed 256-token buffer. Sources longer than the limit are truncated
       safely at the last complete token. */
    tokens[(*count)++] = (ernx_token_t){ .type = ERNX_TOK_EOF };
}

static int32_t ernx_var_get(const char* name) {
    for (int i = 0; i < g_ernx_ctx.num_vars; i++) {
        if (str_eq(g_ernx_ctx.var_names[i], name)) return g_ernx_ctx.var_values[i];
    }
    return 0;
}

static void ernx_var_set(const char* name, int32_t val) {
    for (int i = 0; i < g_ernx_ctx.num_vars; i++) {
        if (str_eq(g_ernx_ctx.var_names[i], name)) { g_ernx_ctx.var_values[i] = val; return; }
    }
    if (g_ernx_ctx.num_vars < 64) {
        copy_name(g_ernx_ctx.var_names[g_ernx_ctx.num_vars], name);
        g_ernx_ctx.var_values[g_ernx_ctx.num_vars] = val;
        g_ernx_ctx.num_vars++;
    }
}

static ernx_token_t ernx_parser_peek(ernx_parser_t* p) {
    if (p->idx < p->count) return p->tokens[p->idx];
    return (ernx_token_t){ .type = ERNX_TOK_EOF };
}

static ernx_token_t ernx_parser_next(ernx_parser_t* p) {
    ernx_token_t t = ernx_parser_peek(p);
    p->idx++;
    return t;
}

static int32_t ernx_eval_primary(ernx_parser_t* p);
static int32_t ernx_eval_mul_div(ernx_parser_t* p);
static int32_t ernx_eval_add_sub(ernx_parser_t* p);
static int32_t ernx_eval_cmp(ernx_parser_t* p);
static int32_t ernx_eval_and(ernx_parser_t* p);
static int32_t ernx_eval_or(ernx_parser_t* p);

/* blocks (via hlt, same trick as sleep_ms) reading scancodes straight off
   the keyboard ring buffer until Enter, echoing digits as they're typed,
   and returns the integer typed. Only called from ernxscript_run's own
   call chain, which always runs outside interrupt context (see the
   keyboard_irq_handler comment above) - so hlt-waiting here for more
   keys to arrive is safe, exactly like sleep_ms waiting for more ticks.
   Deliberately simple: digits and a leading '-' only, no shifted-symbol
   handling, since that's all a numeric `input` needs. */
static int32_t ernx_read_line_int(void) {
    char buf[16];
    int len = 0;
    for (;;) {
        while (kbd_buf_tail == kbd_buf_head) {
            __asm__ volatile ("hlt");
        }
        uint8_t scancode = kbd_buffer[kbd_buf_tail];
        kbd_buf_tail = (kbd_buf_tail + 1) % KBD_BUF_SIZE;
        if (scancode & 0x80) continue; /* key release, ignore */
        if (scancode >= 128) continue; /* extended-key prefix */
        char c = scancode_ascii[scancode];
        if (c == '\n') { terminal_putchar('\n'); break; }
        if (c == '\b') {
            if (len > 0) { len--; terminal_putchar('\b'); }
            continue;
        }
        if ((c == '-' && len == 0) || (c >= '0' && c <= '9')) {
            if (len < (int) sizeof(buf) - 1) {
                buf[len++] = c;
                terminal_putchar(c);
            }
        }
    }
    buf[len] = '\0';
    int neg = (len > 0 && buf[0] == '-');
    int32_t val = 0;
    for (int i = neg ? 1 : 0; i < len; i++) val = val * 10 + (buf[i] - '0');
    return neg ? -val : val;
}

static int32_t ernx_eval_primary(ernx_parser_t* p) {
    ernx_token_t t = ernx_parser_peek(p);
    if (t.type == ERNX_TOK_NUMBER) {
        ernx_parser_next(p);
        return t.val.num;
    } else if (t.type == ERNX_TOK_STRING) {
        ernx_parser_next(p);
        terminal_writestring(t.val.str);
        return 0;
    } else if (t.type == ERNX_TOK_IDENT) {
        ernx_parser_next(p);
        return ernx_var_get(t.val.str);
    } else if (t.type == ERNX_TOK_LPAREN) {
        ernx_parser_next(p);
        int32_t v = ernx_eval_expr(p);
        if (ernx_parser_peek(p).type == ERNX_TOK_RPAREN) ernx_parser_next(p);
        return v;
    } else if (t.type == ERNX_TOK_NOT) {
        ernx_parser_next(p);
        return !ernx_eval_primary(p);
    } else if (t.type == ERNX_TOK_MINUS) {
        ernx_parser_next(p);
        return -ernx_eval_primary(p);
    } else if (t.type == ERNX_TOK_RANDOM) {
        /* `random N` or `random(N)` -> uniform integer in [0, N-1] */
        ernx_parser_next(p);
        int32_t n;
        if (ernx_parser_peek(p).type == ERNX_TOK_LPAREN) {
            ernx_parser_next(p);
            n = ernx_eval_expr(p);
            if (ernx_parser_peek(p).type == ERNX_TOK_RPAREN) ernx_parser_next(p);
        } else {
            n = ernx_eval_primary(p);
        }
        if (g_ernx_rand_state == 0) g_ernx_rand_state = pit_ticks | 1u; /* seed once, never 0 */
        uint32_t x = g_ernx_rand_state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;   /* xorshift32 */
        g_ernx_rand_state = x;
        if (n <= 0) return 0;
        return (int32_t) (x % (uint32_t) n);
    } else if (t.type == ERNX_TOK_INPUT) {
        ernx_parser_next(p);
        return ernx_read_line_int();
    }
    return 0;
}

static int32_t ernx_eval_mul_div(ernx_parser_t* p) {
    int32_t v = ernx_eval_primary(p);
    while (1) {
        ernx_token_type_t op = ernx_parser_peek(p).type;
        if (op == ERNX_TOK_MUL) { ernx_parser_next(p); v *= ernx_eval_primary(p);
        } else if (op == ERNX_TOK_DIV) { ernx_parser_next(p); int32_t r = ernx_eval_primary(p); if (r) v /= r;
        } else if (op == ERNX_TOK_MOD) { ernx_parser_next(p); int32_t r = ernx_eval_primary(p); if (r) v %= r;
        } else break;
    }
    return v;
}

static int32_t ernx_eval_add_sub(ernx_parser_t* p) {
    int32_t v = ernx_eval_mul_div(p);
    while (1) {
        ernx_token_type_t op = ernx_parser_peek(p).type;
        if (op == ERNX_TOK_PLUS) { ernx_parser_next(p); v += ernx_eval_mul_div(p);
        } else if (op == ERNX_TOK_MINUS) { ernx_parser_next(p); v -= ernx_eval_mul_div(p);
        } else break;
    }
    return v;
}

static int32_t ernx_eval_cmp(ernx_parser_t* p) {
    int32_t v = ernx_eval_add_sub(p);
    ernx_token_type_t op = ernx_parser_peek(p).type;
    if (op == ERNX_TOK_LT) { ernx_parser_next(p); return v < ernx_eval_add_sub(p);
    } else if (op == ERNX_TOK_GT) { ernx_parser_next(p); return v > ernx_eval_add_sub(p);
    } else if (op == ERNX_TOK_LE) { ernx_parser_next(p); return v <= ernx_eval_add_sub(p);
    } else if (op == ERNX_TOK_GE) { ernx_parser_next(p); return v >= ernx_eval_add_sub(p);
    } else if (op == ERNX_TOK_EQ) { ernx_parser_next(p); return v == ernx_eval_add_sub(p);
    } else if (op == ERNX_TOK_NE) { ernx_parser_next(p); return v != ernx_eval_add_sub(p);
    }
    return v;
}

static int32_t ernx_eval_and(ernx_parser_t* p) {
    int32_t v = ernx_eval_cmp(p);
    while (ernx_parser_peek(p).type == ERNX_TOK_AND) { ernx_parser_next(p); v = v && ernx_eval_cmp(p); }
    return v;
}

static int32_t ernx_eval_or(ernx_parser_t* p) {
    int32_t v = ernx_eval_and(p);
    while (ernx_parser_peek(p).type == ERNX_TOK_OR) { ernx_parser_next(p); v = v || ernx_eval_and(p); }
    return v;
}

static int32_t ernx_eval_expr(ernx_parser_t* p) {
    return ernx_eval_or(p);
}

static void ernx_eval_block(ernx_parser_t* p);
static void ernx_eval_stmt(ernx_parser_t* p);

static void ernx_eval_block(ernx_parser_t* p) {
    while (ernx_parser_peek(p).type != ERNX_TOK_END && ernx_parser_peek(p).type != ERNX_TOK_EOF
           && !g_ernx_break) {
        ernx_eval_stmt(p);
    }
    /* a `break` inside this block stops the statement loop above before
       the parser reaches this block's own END - it's still sitting
       wherever the break statement left it, possibly with un-executed
       statements (and whole nested if/while blocks) still ahead of it.
       Skip forward past all of that, respecting nesting, so the parser
       ends up exactly where the normal (non-break) path below expects
       it: sitting on this block's own matching END. The caller (the
       WHILE handler in ernx_eval_stmt) is what actually clears the flag
       once it's seen it. */
    if (g_ernx_break) {
        int depth = 0;
        while (ernx_parser_peek(p).type != ERNX_TOK_EOF) {
            ernx_token_type_t tt = ernx_parser_peek(p).type;
            if (tt == ERNX_TOK_IF || tt == ERNX_TOK_WHILE) depth++;
            if (tt == ERNX_TOK_END) {
                if (depth == 0) break; /* this END belongs to us */
                depth--;
            }
            ernx_parser_next(p);
        }
    }
    if (ernx_parser_peek(p).type == ERNX_TOK_END) ernx_parser_next(p);
}

static void ernx_eval_stmt(ernx_parser_t* p) {
    ernx_token_t t = ernx_parser_peek(p);
    
    if (t.type == ERNX_TOK_VAR) {
        ernx_parser_next(p);
        ernx_token_t name = ernx_parser_next(p);
        if (ernx_parser_peek(p).type == ERNX_TOK_ASSIGN) {
            ernx_parser_next(p);
            int32_t val = ernx_eval_expr(p);
            ernx_var_set(name.val.str, val);
        }
    } else if (t.type == ERNX_TOK_IDENT) {
        char name[32];
        copy_name(name, t.val.str);
        ernx_parser_next(p);
        if (ernx_parser_peek(p).type == ERNX_TOK_ASSIGN) {
            ernx_parser_next(p);
            ernx_var_set(name, ernx_eval_expr(p));
        }
    } else if (t.type == ERNX_TOK_PRINT) {
        ernx_parser_next(p);
        /* String literals are already terminal output values. Do not feed
           them through the integer evaluator, which would otherwise print
           the string and then append a surprising `0`. */
        if (ernx_parser_peek(p).type == ERNX_TOK_STRING) {
            ernx_token_t str = ernx_parser_next(p);
            terminal_writestring(str.val.str);
        } else {
            print_int((int) ernx_eval_expr(p));
        }
    } else if (t.type == ERNX_TOK_SHELL) {
        ernx_parser_next(p);
        ernx_token_t cmd = ernx_parser_next(p);
        if (cmd.type == ERNX_TOK_STRING) run_command_ex(cmd.val.str, 0);
    } else if (t.type == ERNX_TOK_IF) {
        ernx_parser_next(p);
        int32_t cond = ernx_eval_expr(p);
        if (cond) {
            ernx_eval_block(p);
        } else {
            /* depth counts ANY nested block opener (if or while), since
               both close with a plain END - counting only ERNX_TOK_IF
               here (the original code) meant a nested `while ... end`
               inside a false `if` branch would hand its own END to this
               loop uncounted, closing the outer if one END too early and
               leaving the rest of the script parsed from the wrong
               position. */
            int depth = 1;
            while (depth > 0 && ernx_parser_peek(p).type != ERNX_TOK_EOF) {
                ernx_token_type_t tt = ernx_parser_peek(p).type;
                if (tt == ERNX_TOK_IF || tt == ERNX_TOK_WHILE) depth++;
                if (tt == ERNX_TOK_END) depth--;
                ernx_parser_next(p);
            }
        }
    } else if (t.type == ERNX_TOK_BREAK) {
        ernx_parser_next(p);
        g_ernx_break = 1;
    } else if (t.type == ERNX_TOK_WHILE) {
        ernx_parser_next(p);
        int start_idx = p->idx - 1;
        while (1) {
            p->idx = start_idx + 1;
            int32_t cond = ernx_eval_expr(p);
            if (!cond) {
                int depth = 1;
                while (depth > 0 && ernx_parser_peek(p).type != ERNX_TOK_EOF) {
                    ernx_token_type_t tt = ernx_parser_peek(p).type;
                    if (tt == ERNX_TOK_IF || tt == ERNX_TOK_WHILE) depth++;
                    if (tt == ERNX_TOK_END) depth--;
                    ernx_parser_next(p);
                }
                break;
            }
            ernx_eval_block(p);
            if (g_ernx_break) { g_ernx_break = 0; break; } /* `break` unwound us here - stop the loop, the parser is already positioned right after our matching END */
            p->idx = start_idx + 1;
        }
    } else if (t.type != ERNX_TOK_EOF && t.type != ERNX_TOK_END) {
        ernx_parser_next(p);
    }
}

/* ERNXscript interpreter - the actual function definition.
 *
 * `tokens` used to be a 256-entry local array here. sizeof(ernx_token_t)
 * is 260 bytes (the 256-byte string union dominates it), so that local
 * array alone was ~66 KB - more than 4x the entire 16 KB boot stack
 * (see boot.s) that this function actually runs on (kernel_main ->
 * run_command -> cmd_run -> ernxscript_run, all on task 0's stack,
 * never inside interrupt context). Every `run <file>.ernx` blew straight
 * through the stack into whatever memory happened to sit below it.
 * Static storage fixes it: this function is never called re-entrantly
 * (ERNXscript has no way to run another script from within a script),
 * so one shared buffer is safe and costs .bss space instead of stack. */
static ernx_token_t g_ernx_tokens[256];

static void ernxscript_run(const char* source) {
    int token_count = 0;
    g_ernx_ctx.num_vars = 0;  /* reset script context for each run */
    g_ernx_break = 0;
    
    ernx_tokenize(source, g_ernx_tokens, &token_count);
    
    ernx_parser_t p = { g_ernx_tokens, token_count, 0 };
    while (ernx_parser_peek(&p).type != ERNX_TOK_EOF) {
        ernx_eval_stmt(&p);
    }
}

/* ============= End ERNXscript Interpreter Definition ============= */


void cmd_run(const char* filename) {
    int idx = find_path(filename);
    if (idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(filename);
        terminal_writestring("\n");
        return;
    }
    
    if (files[idx].is_dir) {
        terminal_writestring("That is a directory.\n");
        return;
    }
    
    /* read entire file into a buffer. Static, not a local: this used to
       be a 4 KB local array stacked on top of ernxscript_run's own
       (formerly 66 KB, now static) token buffer, on a 16 KB stack -
       moved to static storage for the same reason. See ernxscript_run. */
    static char source[4096];
    uint32_t size = files[idx].size;
    if (size >= sizeof(source)) size = sizeof(source) - 1;
    
    for (uint32_t i = 0; i < size; i++) {
        source[i] = (char) files[idx].data[i];
    }
    source[size] = '\0';
    
    terminal_writestring("Running: ");
    terminal_writestring(filename);
    terminal_writestring("\n");
    
    ernxscript_run(source);
    
    terminal_writestring("\n");
}

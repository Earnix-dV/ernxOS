#include "util.h"
#include "vga.h"

/* ---------------- tiny string helpers (no libc available) ---------------- */

int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

/* itoa for small non-negative ints (mouse coords are clamped, so this is enough) */
void print_int(int value) {
    char buf[8];
    int i = 0;
    if (value == 0) { terminal_putchar('0'); return; }
    if (value < 0) { terminal_putchar('-'); value = -value; }
    while (value > 0 && i < 7) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }
    while (i > 0) terminal_putchar(buf[--i]);
}

void print_hex(uint32_t value) {
    const char* digits = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        terminal_putchar(digits[(value >> shift) & 0xF]);
    }
}

/* zero-fills a byte buffer (used instead of memset, which isn't available
   without libc). */
void mem_zero(uint8_t* buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
}

/* writes a 32-bit value into `buf` at `offset`, little-endian - used for
   every multi-byte field in the on-disk superblock/file table. */
void write_u32_le(uint8_t* buf, uint32_t offset, uint32_t val) {
    buf[offset]     = (uint8_t) (val & 0xFF);
    buf[offset + 1] = (uint8_t) ((val >> 8) & 0xFF);
    buf[offset + 2] = (uint8_t) ((val >> 16) & 0xFF);
    buf[offset + 3] = (uint8_t) ((val >> 24) & 0xFF);
}

/* reads a little-endian 32-bit value from `buf` at `offset` - inverse of
   write_u32_le. */
uint32_t read_u32_le(const uint8_t* buf, uint32_t offset) {
    return (uint32_t) buf[offset] |
           ((uint32_t) buf[offset + 1] << 8) |
           ((uint32_t) buf[offset + 2] << 16) |
           ((uint32_t) buf[offset + 3] << 24);
}

/* copies up to 31 characters of a null-terminated name into a fixed 32-byte
   destination buffer, always null-terminating the result. Used everywhere a
   file/dir/window name gets stored into a fixed-size buffer. */
void copy_name(char* dest, const char* src) {
    int i = 0;
    while (src[i] != '\0' && i < 31) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

/* command dispatch helpers: match a command word at the start of `cmd`.
   match_arg requires the word be followed by a space and returns a pointer
   to the (possibly empty) argument text after it, or NULL if `cmd` doesn't
   start with "word ". match_arg_opt is the same but also matches the word
   on its own with no trailing space (returning a pointer to the terminating
   '\0' as the empty argument) - for commands whose argument is optional. */
const char* match_arg(const char* cmd, const char* word) {
    int i = 0;
    while (word[i] != '\0') {
        if (cmd[i] != word[i]) return 0;
        i++;
    }
    if (cmd[i] != ' ') return 0;
    return cmd + i + 1;
}

const char* match_arg_opt(const char* cmd, const char* word) {
    int i = 0;
    while (word[i] != '\0') {
        if (cmd[i] != word[i]) return 0;
        i++;
    }
    if (cmd[i] == ' ') return cmd + i + 1;
    if (cmd[i] == '\0') return cmd + i;
    return 0;
}

/* writes the base-10 representation of val (including a leading '-' for
   negatives) into buf, always null-terminating. buf must be at least 12
   bytes (enough for the sign, INT32_MIN's 10 digits, and the null). */
void int_to_str(int32_t val, char* buf) {
    char tmp[11];
    int ti = 0;
    int neg = 0;
    uint32_t uval;

    if (val < 0) {
        neg = 1;
        /* negate via unsigned arithmetic so INT32_MIN (which has no
           positive int32_t counterpart) converts correctly too */
        uval = (uint32_t) (-(val + 1)) + 1;
    } else {
        uval = (uint32_t) val;
    }

    if (uval == 0) {
        tmp[ti++] = '0';
    } else {
        while (uval > 0 && ti < 11) {
            tmp[ti++] = (char) ('0' + (uval % 10));
            uval /= 10;
        }
    }

    int bi = 0;
    if (neg) buf[bi++] = '-';
    while (ti > 0) buf[bi++] = tmp[--ti];
    buf[bi] = '\0';
}

/* parses a base-10 integer (optionally signed) from a null-terminated
   string. No overflow checking - callers are expected to bound how many
   digits can be typed in before this is reached. Non-digit characters
   after a valid prefix are ignored (stops at the first one). */
int32_t str_to_int(const char* s) {
    int i = 0;
    int neg = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    int32_t val = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        val = val * 10 + (s[i] - '0');
        i++;
    }
    return neg ? -val : val;
}

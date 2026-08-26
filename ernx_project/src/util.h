#ifndef ERNXOS_UTIL_H
#define ERNXOS_UTIL_H

#include <stdint.h>

/* ---------------- tiny string helpers (no libc available) ---------------- */

int str_eq(const char* a, const char* b);

/* itoa for small non-negative ints (mouse coords are clamped, so this is enough) */
void print_int(int value);

/* prints a 32-bit value as 8 hex digits, e.g. 0x0044F000 - used for
   addresses (page faults, CR2), which print_int can't handle (it's
   base-10 and caps out at 7 digits). */
void print_hex(uint32_t value);

/* zero-fills a byte buffer (used instead of memset, which isn't available
   without libc). */
void mem_zero(uint8_t* buf, uint32_t len);

/* writes a 32-bit value into `buf` at `offset`, little-endian - used for
   every multi-byte field in the on-disk superblock/file table. */
void write_u32_le(uint8_t* buf, uint32_t offset, uint32_t val);

/* reads a little-endian 32-bit value from `buf` at `offset` - inverse of
   write_u32_le. */
uint32_t read_u32_le(const uint8_t* buf, uint32_t offset);

/* copies up to 31 characters of a null-terminated name into a fixed 32-byte
   destination buffer, always null-terminating the result. Used everywhere a
   file/dir/window name gets stored into a fixed-size buffer. */
void copy_name(char* dest, const char* src);

/* command dispatch helpers: match a command word at the start of `cmd`.
   match_arg requires the word be followed by a space and returns a pointer
   to the (possibly empty) argument text after it, or NULL if `cmd` doesn't
   start with "word ". match_arg_opt is the same but also matches the word
   on its own with no trailing space (returning a pointer to the terminating
   '\0' as the empty argument) - for commands whose argument is optional. */
/* command dispatch helpers: match a command word at the start of `cmd`.
   match_arg requires the word be followed by a space and returns a pointer
   to the (possibly empty) argument text after it, or NULL if `cmd` doesn't
   start with "word ". match_arg_opt is the same but also matches the word
   on its own with no trailing space (returning a pointer to the terminating
   '\0' as the empty argument) - for commands whose argument is optional. */
const char* match_arg(const char* cmd, const char* word);
const char* match_arg_opt(const char* cmd, const char* word);

/* writes the base-10 representation of val (including a leading '-' for
   negatives) into buf, always null-terminating. buf must be at least 12
   bytes (enough for the sign, INT32_MIN's 10 digits, and the null). Used
   by anything that needs a number as a string rather than printed
   straight to the terminal (print_int only writes to the console). */
void int_to_str(int32_t val, char* buf);

/* parses a base-10 integer (optionally signed) from a null-terminated
   string. No overflow checking - callers are expected to bound how many
   digits can be typed in before this is reached. Non-digit characters
   after a valid prefix are ignored (stops at the first one). */
int32_t str_to_int(const char* s);

#endif /* ERNXOS_UTIL_H */

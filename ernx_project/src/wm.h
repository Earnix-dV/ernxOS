#ifndef ERNXOS_WM_H
#define ERNXOS_WM_H

#include <stdint.h>

/* ---------------- window manager: text-mode windowing system ---------------- */

#define MAX_WINDOWS 5
#define MIN_WIN_WIDTH 20
#define MIN_WIN_HEIGHT 5

typedef struct {
    int row, col;           /* top-left corner */
    int height, width;      /* actual drawable area (inside borders) */
    char title[32];
    char buffer[2000];      /* content: up to ~25 lines * 80 chars */
    int buf_len;            /* how much is filled */
    int scroll_offset;      /* for long content */
    uint8_t color;          /* fg/bg for this window's text */
    int active;             /* 1 if this window has focus */
} window_t;

extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int active_window_idx;

int window_create(int row, int col, int height, int width, const char* title);
void window_focus(int idx);
void window_draw(int idx);
void window_redraw_all(void);
void window_write_n(const char* text, uint32_t len);
void window_write(const char* text);

/* color <fg>/<bg> parsing, shared with the `color` shell command */
int parse_color(const char* tok);

/* shell commands */
void cmd_newwin(char* args);
void cmd_closewin(void);
void cmd_edit(const char* name);
void cmd_color(char* args);
void cmd_win(char* args);

#endif /* ERNXOS_WM_H */

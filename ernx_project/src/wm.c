#include "wm.h"
#include "vga.h"
#include "util.h"
#include "vfs.h"

window_t windows[MAX_WINDOWS];
int window_count = 0;
int active_window_idx = 0;

/* ---------------- colors and simple text-mode windows ---------------- */

static const char* color_names[16] = {
    "black", "blue", "green", "cyan", "red", "magenta", "brown", "lightgrey",
    "darkgrey", "lightblue", "lightgreen", "lightcyan", "lightred",
    "lightmagenta", "yellow", "white"
};

/* accepts a color name ("cyan") or a number ("3"). returns 0-15, or -1 if invalid */
int parse_color(const char* tok) {
    for (int i = 0; i < 16; i++) {
        if (str_eq(tok, color_names[i])) return i;
    }
    if (tok[0] >= '0' && tok[0] <= '9') {
        int v = tok[0] - '0';
        if (tok[1] >= '0' && tok[1] <= '9' && tok[2] == '\0') {
            v = v * 10 + (tok[1] - '0');
        } else if (tok[1] != '\0') {
            return -1;
        }
        if (v >= 0 && v <= 15) return v;
    }
    return -1;
}

/* window manager: creates a new window. returns 1 on success, 0 if max windows reached. */
int window_create(int row, int col, int height, int width, const char* title) {
    if (window_count >= MAX_WINDOWS) return 0;
    if (height < MIN_WIN_HEIGHT || width < MIN_WIN_WIDTH) return 0;
    if (row + height > TERM_HEIGHT || col + width > VGA_WIDTH) return 0;

    window_t* w = &windows[window_count];
    w->row = row;
    w->col = col;
    w->height = height - 2; /* minus top/bottom border */
    w->width = width - 2;   /* minus left/right border */
    w->buf_len = 0;
    w->scroll_offset = 0;
    w->color = (uint8_t) ((0 << 4) | 7); /* white on black */
    w->active = (window_count == 0) ? 1 : 0;

    copy_name(w->title, title);
    mem_zero((uint8_t*) w->buffer, 2000);

    window_count++;
    return 1;
}

/* sets focus to the window at index idx. */
void window_focus(int idx) {
    if (idx < 0 || idx >= window_count) return;
    for (int i = 0; i < window_count; i++) windows[i].active = 0;
    windows[idx].active = 1;
    active_window_idx = idx;
}

/* draws a single window to VGA memory (borders + title + content). */
void window_draw(int idx) {
    if (idx < 0 || idx >= window_count) return;
    window_t* w = &windows[idx];
    uint8_t border_color = w->active ?
        (uint8_t) ((4 << 4) | 15) : /* red on white if active - bright */
        (uint8_t) ((0 << 4) | 8);   /* black on black if inactive */

    /* draw borders */
    for (int x = 0; x < w->width + 2; x++) {
        VGA_MEMORY[(w->row) * VGA_WIDTH + (w->col + x)] = vga_entry_ext(0xC4, border_color);
        VGA_MEMORY[(w->row + w->height + 1) * VGA_WIDTH + (w->col + x)] = vga_entry_ext(0xC4, border_color);
    }
    for (int y = 0; y < w->height + 2; y++) {
        VGA_MEMORY[(w->row + y) * VGA_WIDTH + (w->col)] = vga_entry_ext(0xB3, border_color);
        VGA_MEMORY[(w->row + y) * VGA_WIDTH + (w->col + w->width + 1)] = vga_entry_ext(0xB3, border_color);
    }
    VGA_MEMORY[w->row * VGA_WIDTH + w->col] = vga_entry_ext(0xDA, border_color);
    VGA_MEMORY[w->row * VGA_WIDTH + w->col + w->width + 1] = vga_entry_ext(0xBF, border_color);
    VGA_MEMORY[(w->row + w->height + 1) * VGA_WIDTH + w->col] = vga_entry_ext(0xC0, border_color);
    VGA_MEMORY[(w->row + w->height + 1) * VGA_WIDTH + w->col + w->width + 1] = vga_entry_ext(0xD9, border_color);

    /* title on top border, centered */
    int tlen = 0;
    while (w->title[tlen] != '\0') tlen++;
    if (tlen > w->width - 2) tlen = w->width - 2;
    int start = w->col + 1 + ((w->width - tlen) / 2);
    for (int i = 0; i < tlen; i++) {
        VGA_MEMORY[w->row * VGA_WIDTH + start + i] = vga_entry(w->title[i], border_color);
    }

    /* content area: clear and draw text */
    for (int y = 1; y <= w->height; y++) {
        for (int x = 1; x <= w->width; x++) {
            VGA_MEMORY[(w->row + y) * VGA_WIDTH + (w->col + x)] = vga_entry(' ', w->color);
        }
    }

    /* write buffer content */
    int line = 0, col = 0;
    for (int i = 0; i < w->buf_len && line < w->height; i++) {
        char c = w->buffer[i];
        if (c == '\n') {
            line++;
            col = 0;
        } else {
            if (col < w->width) {
                VGA_MEMORY[(w->row + 1 + line) * VGA_WIDTH + (w->col + 1 + col)] = vga_entry(c, w->color);
                col++;
            }
        }
    }
}

/* redraw all windows (back to front). */
void window_redraw_all(void) {
    for (int i = 0; i < window_count; i++) {
        window_draw(i);
    }
}

/* append exactly `len` bytes to the active window's buffer. Use this (not
   window_write) for data that isn't guaranteed to be null-terminated, such
   as raw file bytes from files[i].data - those are stored with an exact
   size and no trailing '\0', so scanning for one would read past the end
   of the file's buffer into whatever memory follows it. */
void window_write_n(const char* text, uint32_t len) {
    if (window_count == 0 || active_window_idx < 0) return;
    window_t* w = &windows[active_window_idx];
    uint32_t i = 0;
    while (i < len && w->buf_len < 1999) {
        w->buffer[w->buf_len++] = text[i++];
    }
}

/* append a null-terminated C string to the active window's buffer. */
void window_write(const char* text) {
    if (window_count == 0 || active_window_idx < 0) return;
    window_t* w = &windows[active_window_idx];
    int i = 0;
    while (text[i] != '\0' && w->buf_len < 1999) {
        w->buffer[w->buf_len++] = text[i++];
    }
}

/* newwin <title> - creates a demo window. */
void cmd_newwin(char* args) {
    const char* title = (args[0] != '\0') ? args : "Window";
    if (window_create(3, 10, 12, 50, title)) {
        window_focus(window_count - 1);
        window_write("New window opened.\n");
        window_redraw_all();
        terminal_writestring("Created window: ");
        terminal_writestring(title);
        terminal_putchar('\n');
    } else {
        terminal_writestring("Can't create window (max reached or bad size).\n");
    }
}

/* closewin - closes the active window. */
void cmd_closewin(void) {
    if (window_count == 0) {
        terminal_writestring("No windows to close.\n");
        return;
    }
    int closed_idx = active_window_idx;
    for (int i = closed_idx; i < window_count - 1; i++) {
        windows[i] = windows[i + 1];
    }
    window_count--;
    if (window_count > 0) {
        int new_idx = (closed_idx >= window_count) ? window_count - 1 : closed_idx;
        window_focus(new_idx);
    } else {
        active_window_idx = 0;
    }
    terminal_clear();
    window_redraw_all();
    terminal_writestring("Window closed.\n> ");
}

/* edit <filename> - opens file in a new window (read-only viewer for now). */
void cmd_edit(const char* name) {
    if (name[0] == '\0') {
        terminal_writestring("Usage: edit <filename>\n");
        return;
    }
    int idx = find_path(name);
    if (idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(name);
        terminal_putchar('\n');
        return;
    }
    if (files[idx].is_dir) {
        terminal_writestring("That's a directory.\n");
        return;
    }
    if (!window_create(2, 5, 18, 70, name)) {
        terminal_writestring("Can't open editor (max windows reached).\n");
        return;
    }
    window_focus(window_count - 1);
    window_write_n((const char*) files[idx].data, files[idx].size);
    window_redraw_all();
    terminal_writestring("Opened file in new window: ");
    terminal_writestring(name);
    terminal_putchar('\n');
}

/* color <fg> <bg> - changes the color new text is written in */
void cmd_color(char* args) {
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ') i++;
    if (args[i] == '\0') {
        terminal_writestring("Usage: color <fg> <bg>  (e.g. color yellow blue)\n");
        terminal_writestring("Names: black blue green cyan red magenta brown lightgrey\n");
        terminal_writestring("       darkgrey lightblue lightgreen lightcyan lightred\n");
        terminal_writestring("       lightmagenta yellow white  (or numbers 0-15)\n");
        return;
    }
    args[i] = '\0';
    char* fg_tok = args;
    char* bg_tok = args + i + 1;

    int fg = parse_color(fg_tok);
    int bg = parse_color(bg_tok);
    if (fg == -1 || bg == -1) {
        terminal_writestring("Unknown color. Try 'color' with no arguments to see the list.\n");
        return;
    }
    term_color = (uint8_t) ((bg << 4) | fg);
    terminal_writestring("Color set.\n");
}

/* draws a bordered box directly into VGA memory without touching the cursor,
   using CP437 box-drawing bytes (single line: corners, horizontal, vertical) */
static void draw_window(int row, int col, int width, int height, const char* title, uint8_t color) {
    if (width < 2 || height < 2) return;
    if (row < 0 || col < 0 || row + height > TERM_HEIGHT || col + width > VGA_WIDTH) return;

    VGA_MEMORY[row * VGA_WIDTH + col] = vga_entry_ext(0xDA, color);                       /* top-left  */
    VGA_MEMORY[row * VGA_WIDTH + col + width - 1] = vga_entry_ext(0xBF, color);           /* top-right */
    VGA_MEMORY[(row + height - 1) * VGA_WIDTH + col] = vga_entry_ext(0xC0, color);        /* bot-left  */
    VGA_MEMORY[(row + height - 1) * VGA_WIDTH + col + width - 1] = vga_entry_ext(0xD9, color); /* bot-right */

    for (int x = 1; x < width - 1; x++) {
        VGA_MEMORY[row * VGA_WIDTH + col + x] = vga_entry_ext(0xC4, color);
        VGA_MEMORY[(row + height - 1) * VGA_WIDTH + col + x] = vga_entry_ext(0xC4, color);
    }
    for (int y = 1; y < height - 1; y++) {
        VGA_MEMORY[(row + y) * VGA_WIDTH + col] = vga_entry_ext(0xB3, color);
        VGA_MEMORY[(row + y) * VGA_WIDTH + col + width - 1] = vga_entry_ext(0xB3, color);
        for (int x = 1; x < width - 1; x++) {
            VGA_MEMORY[(row + y) * VGA_WIDTH + col + x] = vga_entry(' ', color);
        }
    }

    /* title, centered on the top border */
    int tlen = 0;
    while (title[tlen] != '\0') tlen++;
    int max_title = width - 4;
    if (max_title < 0) max_title = 0; /* window too narrow for any title text */
    if (tlen > max_title) tlen = max_title; /* clip so it never spills past the corners */
    int start = col + 1 + ((width - 2 - tlen) / 2);
    for (int i = 0; i < tlen; i++) {
        VGA_MEMORY[row * VGA_WIDTH + start + i] = vga_entry(title[i], color);
    }
}

/* win [title] - draws a demo window near the top of the screen. Uses raw VGA
   writes, so it doesn't move the cursor — run it right after 'clear' for the
   cleanest result, since it doesn't push existing text out of the way. */
void cmd_win(char* args) {
    const char* title = (args[0] != '\0') ? args : "Window";
    uint8_t win_color = (uint8_t) ((1 << 4) | 15); /* white on blue */
    draw_window(2, 20, 40, 10, title, win_color);
    terminal_writestring("Drew a window (best right after 'clear').\n");
}

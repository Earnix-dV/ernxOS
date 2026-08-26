#include "shell.h"
#include "vga.h"
#include "util.h"
#include "fs.h"
#include "wm.h"
#include "hw.h"
#include "ernxscript.h"
#include "gfx.h"

void run_command_ex(char* cmd, int interactive) {
    if (interactive) terminal_putchar('\n');
    const char* arg;
    if (str_eq(cmd, "help")) {
        terminal_writestring("Commands: help, hi, clear, ls, cat <file>,\n");
        terminal_writestring("          mkdir <name>, touch <name>, write <name> <text>,\n");
        terminal_writestring("          delete <name> (or rm <name>),\n");
        terminal_writestring("          rename <old> <new>, copy <src> <dst>, search <text>,\n");
        terminal_writestring("          move <src> <dst> (or mv), mkdir dir/sub for nested dirs,\n");
        terminal_writestring("          beep [freq_hz], time, uptime, reboot,\n");
        terminal_writestring("          color <fg> <bg>, win [title],\n");
        terminal_writestring("          newwin <title>, closewin, edit <file>, run <script.ernx>\n");
        terminal_writestring("          gfx (graphics mode demo, ESC to return)\n");
        terminal_writestring("(use Alt+Tab to switch windows, click to focus)\n");
    } else if (str_eq(cmd, "hi")) {
        terminal_writestring("Hello to you too!\n");
    } else if (str_eq(cmd, "clear")) {
        terminal_clear();
    } else if ((arg = match_arg_opt(cmd, "ls"))) {
        cmd_ls(arg);
    } else if ((arg = match_arg(cmd, "cat"))) {
        cmd_cat(arg);
    } else if ((arg = match_arg(cmd, "mkdir"))) {
        cmd_mkdir(arg);
    } else if ((arg = match_arg(cmd, "touch"))) {
        cmd_touch(arg);
    } else if ((arg = match_arg(cmd, "write"))) {
        cmd_write((char*) arg);
    } else if ((arg = match_arg_opt(cmd, "delete"))) {
        cmd_delete(arg);
    } else if ((arg = match_arg_opt(cmd, "rm"))) {
        cmd_delete(arg);
    } else if ((arg = match_arg(cmd, "rename"))) {
        cmd_rename((char*) arg);
    } else if ((arg = match_arg(cmd, "move"))) {
        cmd_move((char*) arg);
    } else if ((arg = match_arg(cmd, "mv"))) {
        cmd_move((char*) arg);
    } else if ((arg = match_arg(cmd, "copy"))) {
        cmd_copy((char*) arg);
    } else if ((arg = match_arg_opt(cmd, "search"))) {
        cmd_search(arg);
    } else if ((arg = match_arg_opt(cmd, "beep"))) {
        cmd_beep((char*) arg);
    } else if (str_eq(cmd, "time")) {
        cmd_time();
    } else if (str_eq(cmd, "uptime")) {
        cmd_uptime();
    } else if (str_eq(cmd, "reboot")) {
        cmd_reboot();
    } else if ((arg = match_arg(cmd, "run"))) {
        cmd_run(arg);
    } else if ((arg = match_arg_opt(cmd, "color"))) {
        cmd_color((char*) arg);
    } else if ((arg = match_arg_opt(cmd, "win"))) {
        cmd_win((char*) arg);
    } else if ((arg = match_arg_opt(cmd, "newwin"))) {
        cmd_newwin((char*) arg);
    } else if (str_eq(cmd, "closewin")) {
        cmd_closewin();
    } else if ((arg = match_arg(cmd, "edit"))) {
        cmd_edit(arg);
    } else if (str_eq(cmd, "gfx")) {
        cmd_gfx();
    } else if (cmd[0] == '\0') {
        /* empty line, do nothing */
    } else {
        terminal_writestring("Unknown command: ");
        terminal_writestring(cmd);
        terminal_putchar('\n');
    }
    if (interactive) terminal_writestring("> ");
}

/* the ordinary entry point every other caller in this file should use -
   always interactive, matching the original run_command's behavior. */
void run_command(char* cmd) {
    run_command_ex(cmd, 1);
}

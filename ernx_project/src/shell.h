#ifndef ERNXOS_SHELL_H
#define ERNXOS_SHELL_H

/* `interactive` controls the two pieces of decoration that only make
   sense for the human typing at the shell prompt: the leading blank line
   (visually separating the command from what's typed) and the trailing
   "> " prompt. ERNXscript's `shell "..."` calls this with interactive=0,
   since a script running its own beeps and prints doesn't want a stray
   "> " prompt appearing in the middle of its output every time it shells
   out. */
void run_command_ex(char* cmd, int interactive);

/* the ordinary entry point every other caller should use - always
   interactive, matching the original run_command's behavior. */
void run_command(char* cmd);

#endif /* ERNXOS_SHELL_H */

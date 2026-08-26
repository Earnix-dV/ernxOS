#ifndef ERNXOS_ERNXSCRIPT_H
#define ERNXOS_ERNXSCRIPT_H

/* ---------------- ERNXscript: a tiny scripting language for .ernx files ----------------
   Supports variables, if/while, print, shell "<command>", input, random,
   and basic arithmetic/comparison/boolean operators. Interpreted directly
   from a flat token array - no bytecode, no AST allocation. */

/* `run <file.ernx>` shell command - loads, tokenizes and runs the script. */
void cmd_run(const char* filename);

#endif /* ERNXOS_ERNXSCRIPT_H */

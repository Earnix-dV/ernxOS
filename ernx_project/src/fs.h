#ifndef ERNXOS_FS_H
#define ERNXOS_FS_H

#include <stdint.h>

/* ---------------- persistent on-disk filesystem ----------------
   Layout (512-byte sectors):
     sector 0        superblock: magic[8] "MYOSFS01", file_count (u32), next_free_sector (u32)
     sectors 1..3    file table: up to 32 entries x 48 bytes = 1536 bytes
     sector 4..      file data, allocated sequentially, never reclaimed (no delete yet)

   Files created with mkdir/touch/write are written straight to disk (write-through)
   and re-loaded into RAM automatically on every boot, so they survive a reboot. */

extern int disk_file_count;
extern uint32_t disk_next_free_sector;
extern int disk_ready;

/* loads (or formats) the on-disk filesystem, merging persisted files into
   the RAM file table. Call once at boot, after ata_identify()-capable
   disk.c is available. */
void disk_init(void);

/* shell commands */
void cmd_ls(const char* path);
void cmd_cat(const char* name);
void cmd_mkdir(const char* path);
void cmd_touch(const char* path);
void cmd_write(char* args);
void cmd_delete(const char* path);
void cmd_rename(char* args);
void cmd_move(char* args);
void cmd_copy(char* args);
void cmd_search(const char* term);

#endif /* ERNXOS_FS_H */

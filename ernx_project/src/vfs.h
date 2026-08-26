#ifndef ERNXOS_VFS_H
#define ERNXOS_VFS_H

#include <stdint.h>

/* ---------------- simple initrd (file archive loaded by GRUB) + in-RAM
   file table shared by initrd files and persisted disk files ---------------- */

typedef struct {
    char name[32];
    uint32_t size;
    const uint8_t* data;
    int is_dir;
    int8_t parent; /* index into files[] of the parent directory, or -1 for
                      the root - what makes nested folders possible */
} file_entry_t;

#define MAX_FILES 32
extern file_entry_t files[MAX_FILES];
extern int file_count;

/* simple bump-allocated RAM pool backing every file's in-memory copy
   (initrd files, and anything created with mkdir/touch/write). The disk
   driver mirrors persisted files into this pool too, so ls/cat always
   read from RAM - disk is just where things get saved for next boot. */
#define RAMFS_POOL_SIZE 65536
extern uint8_t ramfs_pool[RAMFS_POOL_SIZE];
extern uint32_t ramfs_used;

uint8_t* ramfs_alloc(uint32_t size);
int find_in_dir(int parent, const char* name);
int resolve_path(const char* path, int* out_parent, char* out_name);
int find_path(const char* path);
void build_path(int idx, char* buf, int buf_size);

void load_initrd(const uint8_t* start, const uint8_t* end);
void multiboot_load_modules(uint32_t magic, uint32_t info_addr);

#endif /* ERNXOS_VFS_H */

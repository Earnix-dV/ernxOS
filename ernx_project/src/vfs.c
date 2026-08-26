#include "vfs.h"
#include "util.h"

file_entry_t files[MAX_FILES];
int file_count = 0;

uint8_t ramfs_pool[RAMFS_POOL_SIZE];
uint32_t ramfs_used = 0;

uint8_t* ramfs_alloc(uint32_t size) {
    if (ramfs_used + size > RAMFS_POOL_SIZE) return 0; /* out of space */
    uint8_t* ptr = &ramfs_pool[ramfs_used];
    ramfs_used += size;
    return ptr;
}

/* like find_file, but scoped to one directory - `parent` is a files[]
   index (or -1 for root). Needed once files in different folders are
   allowed to share a name. */
int find_in_dir(int parent, const char* name) {
    for (int i = 0; i < file_count; i++) {
        if (files[i].parent == parent && str_eq(files[i].name, name)) return i;
    }
    return -1;
}

/* splits a '/'-separated path (e.g. "docs/notes/todo.txt") and walks it
   from the root, requiring every component except the last to already
   exist and be a directory. On success, sets *out_parent to the files[]
   index the last component lives in (or -1 for root) and copies the last
   component's name into out_name (a caller-supplied 32-byte buffer) -
   useful both for looking an existing path up (pair with find_in_dir) and
   for figuring out where a brand-new entry (mkdir/touch) should go.
   Returns 0 if a middle component is missing or isn't a directory, or if
   the path is empty. */
int resolve_path(const char* path, int* out_parent, char* out_name) {
    int parent = -1;
    int start = 0;
    while (1) {
        int j = start;
        while (path[j] != '\0' && path[j] != '/') j++;
        int is_last = (path[j] == '\0');

        char component[32];
        int len = j - start;
        if (len > 31) len = 31;
        for (int k = 0; k < len; k++) component[k] = path[start + k];
        component[len] = '\0';

        if (component[0] == '\0') {
            /* empty component from a leading/trailing/doubled '/' - skip it,
               unless the whole path was empty or nothing but slashes */
            if (is_last) {
                if (start == 0) return 0;
                break;
            }
            start = j + 1;
            continue;
        }

        if (is_last) {
            *out_parent = parent;
            copy_name(out_name, component);
            return 1;
        }

        int idx = find_in_dir(parent, component);
        if (idx == -1 || !files[idx].is_dir) return 0;
        parent = idx;
        start = j + 1;
    }
    return 0;
}

/* looks up a full path directly, e.g. find_path("docs/todo.txt"). Returns
   the files[] index, or -1 if any part of the path doesn't exist. */
int find_path(const char* path) {
    int parent;
    char name[32];
    if (!resolve_path(path, &parent, name)) return -1;
    return find_in_dir(parent, name);
}

/* writes the full slash-separated path of files[idx] into buf, walking up
   through parent links. Used by search results, where duplicate names in
   different folders make a bare name ambiguous. */
void build_path(int idx, char* buf, int buf_size) {
    int chain[MAX_FILES];
    int depth = 0;
    while (idx != -1 && depth < MAX_FILES) {
        chain[depth++] = idx;
        idx = files[idx].parent;
    }
    int pos = 0;
    for (int i = depth - 1; i >= 0; i--) {
        const char* name = files[chain[i]].name;
        int k = 0;
        while (name[k] != '\0' && pos < buf_size - 1) buf[pos++] = name[k++];
        if (i > 0 && pos < buf_size - 1) buf[pos++] = '/';
    }
    buf[pos] = '\0';
}

/* archive format written by pack_initrd.py:
   for each file: 32-byte name (null padded) + 4-byte little-endian size + raw data
   the archive ends when a name starts with a 0 byte */
void load_initrd(const uint8_t* start, const uint8_t* end) {
    const uint8_t* p = start;
    file_count = 0;
    while (p + 36 <= end && file_count < MAX_FILES) {
        if (p[0] == 0) break; /* empty name = end of archive */
        file_entry_t* f = &files[file_count];
        for (int i = 0; i < 32; i++) f->name[i] = p[i];
        f->name[31] = '\0';
        uint32_t size = read_u32_le(p, 32);
        f->size = size;
        f->data = p + 36;
        f->is_dir = 0;
        f->parent = -1; /* initrd files always live at the root */
        p = p + 36 + size;
        file_count++;
    }
}

void multiboot_load_modules(uint32_t magic, uint32_t info_addr) {
    if (magic != 0x2BADB002) return; /* not booted by a multiboot loader */
    uint32_t mods_count = *(uint32_t*)(info_addr + 20);
    uint32_t mods_addr  = *(uint32_t*)(info_addr + 24);
    if (mods_count == 0) return;
    uint32_t mod_start = *(uint32_t*)(mods_addr + 0);
    uint32_t mod_end   = *(uint32_t*)(mods_addr + 4);
    load_initrd((const uint8_t*) mod_start, (const uint8_t*) mod_end);
}

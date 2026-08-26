#include "fs.h"
#include "vfs.h"
#include "disk.h"
#include "vga.h"
#include "util.h"

/* ---------------- persistent on-disk filesystem ----------------
   Layout (512-byte sectors):
     sector 0        superblock: magic[8] "MYOSFS01", file_count (u32), next_free_sector (u32)
     sectors 1..3    file table: up to 32 entries x 48 bytes = 1536 bytes
     sector 4..      file data, allocated sequentially, never reclaimed (no delete yet)

   Files created with mkdir/touch/write are written straight to disk (write-through)
   and re-loaded into RAM automatically on every boot, so they survive a reboot. */

#define FS_MAGIC "MYOSFS01"
#define FS_TABLE_START_SECTOR 1
#define FS_TABLE_SECTORS 3
#define FS_DATA_START_SECTOR 4
#define MAX_DISK_FILES 32

typedef struct {
    char name[32];
    uint32_t size;
    uint32_t start_sector;
    uint8_t is_dir;
    int8_t parent; /* index into disk_files (aligned 1:1 with files[] - see
                       disk_init) of the parent directory, or -1 for root */
    uint8_t _pad[6];
} disk_file_entry_t;

static disk_file_entry_t disk_files[MAX_DISK_FILES];
/* disk_files use disk-table indices for parent links on disk, while files[]
   uses RAM indices. This map keeps the two namespaces separate after boot,
   when persisted entries can be merged into initrd entries. */
static int disk_ram_index[MAX_DISK_FILES];
int disk_file_count = 0;
uint32_t disk_next_free_sector = FS_DATA_START_SECTOR;
int disk_ready = 0;

static int disk_index_for_ram(int ram_idx) {
    if (ram_idx < 0) return -1;
    for (int i = 0; i < disk_file_count; i++) {
        if (disk_ram_index[i] == ram_idx) return i;
    }
    return -1;
}

static int disk_find_file(const char* name, int ram_parent) {
    int disk_parent = disk_index_for_ram(ram_parent);
    for (int i = 0; i < disk_file_count; i++) {
        if (disk_files[i].parent == disk_parent && str_eq(disk_files[i].name, name)) return i;
    }
    return -1;
}

/* writes the superblock + whole file table back to disk */
static void disk_save_table(void) {
    if (!disk_ready) return;

    uint8_t super[512];
    mem_zero(super, 512);
    for (int i = 0; i < 8; i++) super[i] = (uint8_t) FS_MAGIC[i];
    write_u32_le(super, 8, (uint32_t) disk_file_count);
    write_u32_le(super, 12, disk_next_free_sector);
    ata_write_sector(0, super);

    uint8_t table[512 * FS_TABLE_SECTORS];
    mem_zero(table, 512 * FS_TABLE_SECTORS);
    for (int i = 0; i < disk_file_count; i++) {
        uint8_t* dst = &table[i * sizeof(disk_file_entry_t)];
        disk_file_entry_t* f = &disk_files[i];
        for (int j = 0; j < 32; j++) dst[j] = (uint8_t) f->name[j];
        write_u32_le(dst, 32, f->size);
        write_u32_le(dst, 36, f->start_sector);
        dst[40] = f->is_dir;
        dst[41] = (uint8_t) f->parent;
    }
    for (int s = 0; s < FS_TABLE_SECTORS; s++) {
        ata_write_sector((uint32_t)(FS_TABLE_START_SECTOR + s), &table[s * 512]);
    }
}

/* persists a zero-size directory or file entry (mkdir/touch share this;
   is_dir picks which). `parent` is the files[]/disk_files[] index of the
   directory it lives in (or -1 for root). */
static void disk_add_entry(const char* name, uint8_t is_dir, int parent) {
    if (!disk_ready || disk_find_file(name, parent) != -1 || disk_file_count >= MAX_DISK_FILES) return;
    int ram_idx = find_in_dir(parent, name);
    if (ram_idx < 0) return;

    int disk_idx = disk_file_count;
    disk_file_entry_t* f = &disk_files[disk_idx];
    copy_name(f->name, name);
    f->size = 0;
    f->start_sector = 0;
    f->is_dir = is_dir;
    f->parent = (int8_t) disk_index_for_ram(parent);
    disk_ram_index[disk_idx] = ram_idx;
    disk_file_count++;
    disk_save_table();
}

/* renames and/or relocates an on-disk entry in place (table only - the data
   sectors it already owns don't move, so this is cheap regardless of file
   size). Used by both rename (new_parent == old_parent) and move (name may
   also stay the same). No-op if the disk isn't ready or the old entry isn't
   on disk (e.g. RAM-only file that never got persisted because the disk was
   full). */
static void disk_update_entry(const char* old_name, int old_parent, const char* new_name, int new_parent) {
    if (!disk_ready) return;
    int idx = disk_find_file(old_name, old_parent);
    if (idx == -1) return;
    copy_name(disk_files[idx].name, new_name);
    disk_files[idx].parent = (int8_t) new_parent;
    disk_save_table();
}

/* writes file contents to disk, creating the entry if it doesn't exist yet (write).
   If the new data fits in the space already allocated to this file, it's
   overwritten in place; only a genuine size increase eats fresh disk space.
   Without this, every single write to the same file (e.g. an updated counter
   or log line) would burn a new chunk of disk forever, even though nothing
   else changed - the disk would fill up far faster than it needed to.
   Returns 1 on success, 0 if the disk doesn't have room - callers must check
   this, since silently allocating past disk_total_sectors would either write
   into nothing or wrap around and corrupt the superblock/table. */
static int disk_write_file(const char* name, const uint8_t* data, uint32_t len, int parent) {
    if (!disk_ready) return 0;
    int idx = disk_find_file(name, parent);
    uint32_t old_size = 0;
    uint32_t old_start = 0;
    if (idx == -1) {
        if (disk_file_count >= MAX_DISK_FILES) return 0;
        idx = disk_file_count;
        int ram_idx = find_in_dir(parent, name);
        if (ram_idx < 0) return 0;
        disk_file_entry_t* f = &disk_files[idx];
        copy_name(f->name, name);
        f->is_dir = 0;
        f->size = 0;
        f->start_sector = 0;
        f->parent = (int8_t) disk_index_for_ram(parent);
        disk_ram_index[idx] = ram_idx;
        disk_file_count++;
    } else {
        old_size = disk_files[idx].size;
        old_start = disk_files[idx].start_sector;
    }

    uint32_t nsectors = (len + 511) / 512;
    uint32_t old_nsectors = (old_size + 511) / 512;

    uint32_t start;
    if (len > 0 && old_start != 0 && nsectors <= old_nsectors) {
        start = old_start; /* fits in the space this file already owns - reuse it */
    } else if (len > 0) {
        if (disk_next_free_sector + nsectors > disk_total_sectors) {
            return 0; /* disk full - refuse rather than write past the end */
        }
        start = disk_next_free_sector; /* genuinely grew - needs fresh space */
        disk_next_free_sector += nsectors;
    } else {
        start = 0;
    }

    uint8_t sector_buf[512];
    for (uint32_t s = 0; s < nsectors; s++) {
        mem_zero(sector_buf, 512);
        uint32_t offset = s * 512;
        uint32_t remaining = len - offset;
        uint32_t chunk = remaining < 512 ? remaining : 512;
        for (uint32_t i = 0; i < chunk; i++) sector_buf[i] = data[offset + i];
        ata_write_sector(start + s, sector_buf);
    }

    disk_files[idx].size = len;
    disk_files[idx].start_sector = start;
    disk_save_table();
    return 1;
}

/* rebuilds the disk's data region and file table from scratch using whatever
   is currently in the RAM file list (files[]). Called after a delete so the
   freed space is actually reclaimed and any gap it left is closed - unlike
   the write-in-place reuse in disk_write_file, this also fixes up files that
   come after the deleted one, so there's no lingering hole on disk. Safe
   because it reads each file's bytes from its RAM copy (already loaded),
   never from the disk locations it's about to overwrite. */
static void disk_rebuild(void) {
    if (!disk_ready) return;

    disk_file_count = 0;
    disk_next_free_sector = FS_DATA_START_SECTOR;

    uint8_t sector_buf[512];
    for (int i = 0; i < file_count && disk_file_count < MAX_DISK_FILES; i++) {
        file_entry_t* rf = &files[i];
        disk_file_entry_t* f = &disk_files[disk_file_count];
        int disk_idx = disk_file_count;
        for (int j = 0; j < 32; j++) f->name[j] = rf->name[j];
        f->is_dir = (uint8_t) rf->is_dir;
        f->parent = (int8_t) (rf->parent < 0 ? -1 : rf->parent);
        disk_ram_index[disk_idx] = i; /* rebuild restores 1:1 table/RAM order */

        if (rf->is_dir || rf->size == 0) {
            f->size = 0;
            f->start_sector = 0;
        } else {
            uint32_t nsectors = (rf->size + 511) / 512;
            /* can't happen in practice (rebuild only ever holds the same
               data or less than what already fit before), but never write
               past the real end of the disk regardless */
            if (disk_next_free_sector + nsectors > disk_total_sectors) {
                f->size = 0;
                f->start_sector = 0;
                disk_file_count++;
                continue;
            }
            uint32_t start = disk_next_free_sector;
            for (uint32_t s = 0; s < nsectors; s++) {
                mem_zero(sector_buf, 512);
                uint32_t offset = s * 512;
                uint32_t remaining = rf->size - offset;
                uint32_t chunk = remaining < 512 ? remaining : 512;
                for (uint32_t k = 0; k < chunk; k++) sector_buf[k] = rf->data[offset + k];
                ata_write_sector(start + s, sector_buf);
            }
            f->size = rf->size;
            f->start_sector = start;
            disk_next_free_sector += nsectors;
        }
        disk_file_count++;
    }

    disk_save_table();
}

void disk_init(void) {
    if (!ata_identify()) {
        disk_ready = 0; /* no disk attached (or not a plain ATA disk) - persistence won't work */
        return;
    }

    uint8_t super[512];
    if (!ata_read_sector(0, super)) {
        disk_ready = 0;
        return;
    }
    disk_ready = 1;

    int magic_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (super[i] != FS_MAGIC[i]) { magic_ok = 0; break; }
    }

    if (!magic_ok) {
        /* first boot with this disk - format it */
        disk_file_count = 0;
        disk_next_free_sector = FS_DATA_START_SECTOR;
        disk_save_table();
        return;
    }

    disk_file_count = (int) read_u32_le(super, 8);
    disk_next_free_sector = read_u32_le(super, 12);
    if (disk_file_count < 0 || disk_file_count > MAX_DISK_FILES ||
        disk_next_free_sector < FS_DATA_START_SECTOR ||
        disk_next_free_sector > disk_total_sectors) {
        disk_file_count = 0;
        disk_next_free_sector = FS_DATA_START_SECTOR;
        disk_save_table();
        return;
    }

    uint8_t table[512 * FS_TABLE_SECTORS];
    for (int s = 0; s < FS_TABLE_SECTORS; s++) {
        ata_read_sector((uint32_t)(FS_TABLE_START_SECTOR + s), &table[s * 512]);
    }

    /* maps each disk-table index to wherever it actually ended up in
       files[] - usually the same index, but an entry that reuses an
       existing initrd slot (see below) can land somewhere else, and any
       later entry whose parent points at it needs the real slot, not the
       disk index. Directories are always written to the table before their
       children (mkdir requires the parent to already exist), so by the
       time entry i's parent is looked up here, that parent's mapping has
       already been filled in. */
    int disk_to_ram[MAX_DISK_FILES];

    for (int i = 0; i < disk_file_count; i++) {
        uint8_t* src = &table[i * sizeof(disk_file_entry_t)];
        disk_file_entry_t* f = &disk_files[i];
        for (int j = 0; j < 32; j++) f->name[j] = (char) src[j];
        f->size = read_u32_le(src, 32);
        f->start_sector = read_u32_le(src, 36);
        f->is_dir = src[40] ? 1 : 0;
        f->parent = (int8_t) src[41];

        /* Reject corrupt parent links before indexing disk_to_ram. */
        if (f->parent != -1 && (f->parent < 0 || f->parent >= i)) {
            disk_file_count = 0;
            disk_next_free_sector = FS_DATA_START_SECTOR;
            disk_save_table();
            return;
        }
        int8_t ram_parent = (f->parent == -1) ? -1 : (int8_t) disk_to_ram[(uint8_t) f->parent];
        if (f->parent != -1 && ram_parent < 0) {
            disk_file_count = 0;
            disk_next_free_sector = FS_DATA_START_SECTOR;
            disk_save_table();
            return;
        }

        /* mirror it into the RAM file list used by ls/cat - the disk copy is
           the user's saved version, so if a file with this name is already
           in the RAM list under the same parent (e.g. shipped in the
           initrd), overwrite it in place instead of appending a second
           entry. Otherwise writing to a file that shares its name with a
           bundled demo file would make 'ls' show it twice, and find_path
           (used by cat/edit/write) would keep resolving to the stale
           initrd copy instead of what the user actually saved. */
        int existing = find_in_dir(ram_parent, f->name);
        file_entry_t* rf;
        if (existing != -1) {
            rf = &files[existing];
        } else {
            if (file_count >= MAX_FILES) { disk_to_ram[i] = -1; continue; }
            rf = &files[file_count];
            file_count++;
        }
        disk_to_ram[i] = (int) (rf - files);
        disk_ram_index[i] = disk_to_ram[i];
        rf->parent = ram_parent;
        for (int j = 0; j < 32; j++) rf->name[j] = f->name[j];
        rf->is_dir = f->is_dir;
        rf->size = f->size;
        if (f->is_dir || f->size == 0) {
            rf->data = ramfs_alloc(0);
        } else {
            uint32_t nsectors = (f->size + 511) / 512;
            /* start_sector > disk_total_sectors is checked before the
               subtraction below on purpose: with both sides unsigned,
               disk_total_sectors - f->start_sector would wrap around to a
               huge value for an out-of-range start_sector (a corrupted or
               hand-edited superblock), and the nsectors > ... comparison
               would then silently pass instead of catching it - letting
               ata_read_sector run off the end of the disk further down. */
            if (f->start_sector < FS_DATA_START_SECTOR ||
                f->start_sector > disk_total_sectors ||
                nsectors > disk_total_sectors - f->start_sector) {
                disk_file_count = 0;
                disk_next_free_sector = FS_DATA_START_SECTOR;
                disk_save_table();
                return;
            }
            uint8_t* buf = ramfs_alloc(f->size);
            if (buf) {
                uint8_t sector_buf[512];
                for (uint32_t s = 0; s < nsectors; s++) {
                    ata_read_sector(f->start_sector + s, sector_buf);
                    uint32_t offset = s * 512;
                    uint32_t remaining = f->size - offset;
                    uint32_t chunk = remaining < 512 ? remaining : 512;
                    for (uint32_t k = 0; k < chunk; k++) buf[offset + k] = sector_buf[k];
                }
                rf->data = buf;
            } else {
                /* out of RAM pool space - fall back to an empty file rather
                   than a NULL data pointer paired with a nonzero size, which
                   would crash the first time cat/edit reads it */
                rf->data = ramfs_alloc(0);
                rf->size = 0;
            }
        }
    }
}


void cmd_ls(const char* path) {
    int parent = -1;
    if (path[0] != '\0') {
        int idx = find_path(path);
        if (idx == -1 || !files[idx].is_dir) {
            terminal_writestring("No such directory: ");
            terminal_writestring(path);
            terminal_putchar('\n');
            return;
        }
        parent = idx;
    }
    int shown = 0;
    for (int i = 0; i < file_count; i++) {
        if (files[i].parent != parent) continue;
        terminal_writestring(files[i].name);
        if (files[i].is_dir) {
            terminal_writestring("/\n");
        } else {
            terminal_writestring("  (");
            print_int((int) files[i].size);
            terminal_writestring(" bytes)\n");
        }
        shown++;
    }
    if (shown == 0) terminal_writestring("(empty)\n");
}

void cmd_mkdir(const char* path) {
    if (path[0] == '\0') {
        terminal_writestring("Usage: mkdir <name>  (or mkdir dir/subdir)\n");
        return;
    }
    int parent;
    char name[32];
    if (!resolve_path(path, &parent, name)) {
        terminal_writestring("No such directory: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (find_in_dir(parent, name) != -1) {
        terminal_writestring("Already exists: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (file_count >= MAX_FILES) {
        terminal_writestring("File table full.\n");
        return;
    }
    file_entry_t* f = &files[file_count];

    copy_name(f->name, name);
    f->size = 0;
    f->data = 0;
    f->is_dir = 1;
    f->parent = (int8_t) parent;
    file_count++;
    disk_add_entry(name, 1, parent);
    terminal_writestring("Created directory: ");
    terminal_writestring(path);
    terminal_putchar('\n');
    if (!disk_ready) terminal_writestring("(no disk attached - won't survive reboot)\n");
}

void cmd_touch(const char* path) {
    if (path[0] == '\0') {
        terminal_writestring("Usage: touch <name>  (or touch dir/name)\n");
        return;
    }
    int parent;
    char name[32];
    if (!resolve_path(path, &parent, name)) {
        terminal_writestring("No such directory: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (find_in_dir(parent, name) != -1) {
        terminal_writestring("Already exists: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (file_count >= MAX_FILES) {
        terminal_writestring("File table full.\n");
        return;
    }
    file_entry_t* f = &files[file_count];
    copy_name(f->name, name);
    f->size = 0;
    f->data = ramfs_alloc(0);
    f->is_dir = 0;
    f->parent = (int8_t) parent;
    file_count++;
    disk_add_entry(name, 0, parent);
    terminal_writestring("Created file: ");
    terminal_writestring(path);
    terminal_putchar('\n');
    if (!disk_ready) terminal_writestring("(no disk attached - won't survive reboot)\n");
}

/* write <name> <text...>  - sets/replaces a file's contents with typed text */
void cmd_write(char* args) {
    /* split off the filename (path) at the first space */
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ') i++;
    if (args[i] == '\0') {
        terminal_writestring("Usage: write <name> <text>\n");
        return;
    }
    args[i] = '\0';

    char* path = args;
    char* text = args + i + 1;

    int idx = find_path(path);
    if (idx == -1) {
        int parent;
        char name[32];
        if (!resolve_path(path, &parent, name)) {
            terminal_writestring("No such directory: ");
            terminal_writestring(path);
            terminal_putchar('\n');
            return;
        }
        if (file_count >= MAX_FILES) {
            terminal_writestring("File table full.\n");
            return;
        }
        idx = file_count;
        file_entry_t* f = &files[idx];
        copy_name(f->name, name);
        f->is_dir = 0;
        f->parent = (int8_t) parent;
        file_count++;
    }
    if (files[idx].is_dir) {
        terminal_writestring("That is a directory.\n");
        return;
    }

    int len = 0;
    while (text[len] != '\0') len++;

    /* Reuse the previous allocation when this file is the newest RAM
       allocation. This prevents repeated `write` calls on the same file from
       consuming the entire bump pool unnecessarily. */
    uint32_t old_end = 0;
    uintptr_t data_addr = (uintptr_t) files[idx].data;
    uintptr_t pool_start = (uintptr_t) ramfs_pool;
    uintptr_t pool_end = pool_start + RAMFS_POOL_SIZE;
    if (data_addr >= pool_start && data_addr <= pool_end) {
        old_end = (uint32_t)(data_addr - pool_start) + files[idx].size;
    }
    if (old_end == ramfs_used) {
        ramfs_used -= files[idx].size;
    }

    uint8_t* buf = ramfs_alloc((uint32_t) len);
    if (!buf) {
        terminal_writestring("Out of RAM disk space.\n");
        return;
    }
    for (int k = 0; k < len; k++) buf[k] = (uint8_t) text[k];

    files[idx].data = buf;
    files[idx].size = (uint32_t) len;

    int saved_to_disk = disk_write_file(files[idx].name, buf, (uint32_t) len, files[idx].parent);

    terminal_writestring("Wrote ");
    print_int(len);
    terminal_writestring(" bytes to ");
    terminal_writestring(path);
    terminal_putchar('\n');
    if (disk_ready && !saved_to_disk) {
        terminal_writestring("Disk is full - kept in RAM only, won't survive reboot.\n");
        terminal_writestring("Try 'delete' on something you don't need.\n");
    }
}

/* delete <name> (alias: rm) - removes a file/directory entry from RAM and
   from disk, and reclaims its space by rebuilding the disk data region from
   whatever's left. Refuses on a non-empty directory rather than silently
   orphaning its contents. No RAM equivalent recompaction happens for
   ramfs_pool - RAM is cheap and it just resets on reboot anyway. */
void cmd_delete(const char* path) {
    if (path[0] == '\0') {
        terminal_writestring("Usage: delete <name>  (alias: rm)\n");
        return;
    }
    int idx = find_path(path);
    if (idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return;
    }
    if (files[idx].is_dir) {
        for (int i = 0; i < file_count; i++) {
            if (files[i].parent == idx) {
                terminal_writestring("Directory not empty: ");
                terminal_writestring(path);
                terminal_putchar('\n');
                return;
            }
        }
    }

    for (int i = idx; i < file_count - 1; i++) {
        files[i] = files[i + 1];
    }
    file_count--;

    /* every parent index pointing past the removed slot needs to shift down
       by one to keep tracking the right entry (nothing pointed AT idx - we
       just confirmed it has no children) */
    for (int i = 0; i < file_count; i++) {
        if (files[i].parent > idx) files[i].parent--;
    }

    disk_rebuild();

    terminal_writestring("Deleted: ");
    terminal_writestring(path);
    terminal_putchar('\n');
    if (!disk_ready) terminal_writestring("(no disk attached - this only affected RAM)\n");
}

/* rename <old> <new> - changes a file/dir's name in place, keeping it in the
   same directory (use 'move' to relocate it). Data isn't touched at all
   (RAM pointer or on-disk sectors), only the name field in both the RAM
   list and (if persisted) the disk table. */
void cmd_rename(char* args) {
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ') i++;
    if (args[i] == '\0') {
        terminal_writestring("Usage: rename <old> <new>\n");
        return;
    }
    args[i] = '\0';
    char* old_path = args;
    char* new_name = args + i + 1;
    if (new_name[0] == '\0') {
        terminal_writestring("Usage: rename <old> <new>\n");
        return;
    }
    for (int k = 0; new_name[k] != '\0'; k++) {
        if (new_name[k] == '/') {
            terminal_writestring("New name can't contain '/' - use 'move' to relocate.\n");
            return;
        }
    }

    int idx = find_path(old_path);
    if (idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(old_path);
        terminal_putchar('\n');
        return;
    }
    int parent = files[idx].parent;
    if (find_in_dir(parent, new_name) != -1) {
        terminal_writestring("Already exists: ");
        terminal_writestring(new_name);
        terminal_putchar('\n');
        return;
    }

    char old_name[32];
    copy_name(old_name, files[idx].name);
    copy_name(files[idx].name, new_name);
    disk_update_entry(old_name, parent, new_name, parent);

    terminal_writestring("Renamed to: ");
    terminal_writestring(new_name);
    terminal_putchar('\n');
}

/* move <src> <dst> - relocates a file/dir. If <dst> names an existing
   directory, src is moved into it keeping its own name; otherwise <dst> is
   treated as the full new path (so move can rename and relocate in one
   step, same as a Unix 'mv'). */
void cmd_move(char* args) {
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ') i++;
    if (args[i] == '\0') {
        terminal_writestring("Usage: move <src> <dst>  (or dst is an existing directory)\n");
        return;
    }
    args[i] = '\0';
    char* src_path = args;
    char* dst_path = args + i + 1;
    if (dst_path[0] == '\0') {
        terminal_writestring("Usage: move <src> <dst>  (or dst is an existing directory)\n");
        return;
    }

    int src_idx = find_path(src_path);
    if (src_idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(src_path);
        terminal_putchar('\n');
        return;
    }

    int new_parent;
    char new_name[32];
    int dst_idx = find_path(dst_path);
    if (dst_idx != -1 && files[dst_idx].is_dir) {
        new_parent = dst_idx;
        copy_name(new_name, files[src_idx].name);
    } else if (!resolve_path(dst_path, &new_parent, new_name)) {
        terminal_writestring("No such directory: ");
        terminal_writestring(dst_path);
        terminal_putchar('\n');
        return;
    }

    if (files[src_idx].is_dir) {
        for (int p = new_parent; p != -1; p = files[p].parent) {
            if (p == src_idx) {
                terminal_writestring("Cannot move a directory inside itself.\n");
                return;
            }
        }
    }
    if (new_parent == files[src_idx].parent && str_eq(new_name, files[src_idx].name)) {
        terminal_writestring("Already there.\n");
        return;
    }
    if (find_in_dir(new_parent, new_name) != -1) {
        terminal_writestring("Already exists: ");
        terminal_writestring(new_name);
        terminal_putchar('\n');
        return;
    }

    char old_name[32];
    copy_name(old_name, files[src_idx].name);
    int old_parent = files[src_idx].parent;

    copy_name(files[src_idx].name, new_name);
    files[src_idx].parent = (int8_t) new_parent;
    disk_update_entry(old_name, old_parent, new_name, new_parent);

    terminal_writestring("Moved to: ");
    terminal_writestring(dst_path);
    terminal_putchar('\n');
}

/* copy <src> <dst> - duplicates a file's contents under a new name and/or
   directory (a fresh RAM allocation, and a fresh disk write so the copy is
   independent of the original - editing one afterward doesn't touch the
   other). Directories have no contents yet, so copying one just creates an
   empty dir with the new name. */
void cmd_copy(char* args) {
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ') i++;
    if (args[i] == '\0') {
        terminal_writestring("Usage: copy <src> <dst>\n");
        return;
    }
    args[i] = '\0';
    char* src_path = args;
    char* dst_path = args + i + 1;
    if (dst_path[0] == '\0') {
        terminal_writestring("Usage: copy <src> <dst>\n");
        return;
    }

    int src_idx = find_path(src_path);
    if (src_idx == -1) {
        terminal_writestring("File not found: ");
        terminal_writestring(src_path);
        terminal_putchar('\n');
        return;
    }

    int parent;
    char dst_name[32];
    if (!resolve_path(dst_path, &parent, dst_name)) {
        terminal_writestring("No such directory: ");
        terminal_writestring(dst_path);
        terminal_putchar('\n');
        return;
    }
    if (find_in_dir(parent, dst_name) != -1) {
        terminal_writestring("Already exists: ");
        terminal_writestring(dst_path);
        terminal_putchar('\n');
        return;
    }
    if (file_count >= MAX_FILES) {
        terminal_writestring("File table full.\n");
        return;
    }

    file_entry_t* src = &files[src_idx];

    if (src->is_dir) {
        file_entry_t* f = &files[file_count];
        copy_name(f->name, dst_name);
        f->size = 0;
        f->data = 0;
        f->is_dir = 1;
        f->parent = (int8_t) parent;
        file_count++;
        disk_add_entry(dst_name, 1, parent);
        terminal_writestring("Copied directory to: ");
        terminal_writestring(dst_path);
        terminal_putchar('\n');
        return;
    }

    uint8_t* buf = ramfs_alloc(src->size);
    if (src->size > 0 && !buf) {
        terminal_writestring("Out of RAM disk space.\n");
        return;
    }
    for (uint32_t k = 0; k < src->size; k++) buf[k] = src->data[k];

    file_entry_t* f = &files[file_count];
    copy_name(f->name, dst_name);
    f->size = src->size;
    f->data = buf;
    f->is_dir = 0;
    f->parent = (int8_t) parent;
    file_count++;

    int saved_to_disk = disk_write_file(dst_name, buf, f->size, parent);

    terminal_writestring("Copied to: ");
    terminal_writestring(dst_path);
    terminal_putchar('\n');
    if (disk_ready && !saved_to_disk) {
        terminal_writestring("Disk is full - kept in RAM only, won't survive reboot.\n");
    }
}

/* returns 1 if `needle` (a null-terminated string) occurs anywhere inside
   the first `haystack_len` bytes of `haystack` - a plain byte-for-byte
   substring scan, used by search for both file names and file contents. */
static int bytes_contain(const uint8_t* haystack, uint32_t haystack_len, const char* needle) {
    int needle_len = 0;
    while (needle[needle_len] != '\0') needle_len++;
    if (needle_len == 0) return 0;
    for (uint32_t i = 0; i + (uint32_t) needle_len <= haystack_len; i++) {
        int j = 0;
        while (j < needle_len && (char) haystack[i + j] == needle[j]) j++;
        if (j == needle_len) return 1;
    }
    return 0;
}

/* search <text> - lists every file/dir whose name contains <text>, plus
   every file (not dir) whose contents contain it, noting which kind of
   match it was. */
void cmd_search(const char* term) {
    if (term[0] == '\0') {
        terminal_writestring("Usage: search <text>\n");
        return;
    }

    int matches = 0;
    for (int i = 0; i < file_count; i++) {
        file_entry_t* f = &files[i];
        int name_len = 0;
        while (f->name[name_len] != '\0') name_len++;

        int name_hit = bytes_contain((const uint8_t*) f->name, (uint32_t) name_len, term);
        int content_hit = (!f->is_dir) && f->size > 0 &&
                           bytes_contain(f->data, f->size, term);

        if (name_hit || content_hit) {
            char path[128];
            build_path(i, path, sizeof(path));
            terminal_writestring(path);
            if (f->is_dir) terminal_writestring("/");
            if (name_hit && content_hit) {
                terminal_writestring("  (name + content match)\n");
            } else if (content_hit) {
                terminal_writestring("  (content match)\n");
            } else {
                terminal_writestring("  (name match)\n");
            }
            matches++;
        }
    }

    if (matches == 0) {
        terminal_writestring("No matches for: ");
        terminal_writestring(term);
        terminal_putchar('\n');
    }
}

void cmd_cat(const char* name) {
    int idx = find_path(name);
    if (idx != -1) {
        if (files[idx].is_dir) {
            terminal_writestring("That is a directory.\n");
            return;
        }
        for (uint32_t j = 0; j < files[idx].size; j++) {
            terminal_putchar((char) files[idx].data[j]);
        }
        terminal_putchar('\n');
        return;
    }
    terminal_writestring("File not found: ");
    terminal_writestring(name);
    terminal_putchar('\n');
}

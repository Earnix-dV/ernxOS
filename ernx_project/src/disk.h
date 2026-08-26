#ifndef ERNXOS_DISK_H
#define ERNXOS_DISK_H

#include <stdint.h>

/* ---------------- ATA PIO disk driver (secondary bus: IDE port 1, device 0) ----------------
   Using the secondary channel keeps the disk off the primary channel, where the
   boot ISO's virtual DVD drive lives (port 0 / device 0). Polling PIO only —
   no IRQs — which is simpler and fine for a small hobby disk. */

/* how many 512-byte sectors the attached disk actually has - learned from the
   drive itself via IDENTIFY DEVICE rather than assumed, so the filesystem
   code can refuse to allocate past the real end of the disk instead of
   silently writing into nothing (or wrapping onto sector 0 and corrupting the
   superblock) once it fills up. 0 until ata_identify() succeeds. */
extern uint32_t disk_total_sectors;

int ata_read_sector(uint32_t lba, uint8_t* buf);
int ata_write_sector(uint32_t lba, const uint8_t* buf);

/* returns 1 and fills disk_total_sectors if a plain ATA disk answered, 0 if
   there's nothing attached on this channel (or it's not a plain ATA disk) */
int ata_identify(void);

#endif /* ERNXOS_DISK_H */

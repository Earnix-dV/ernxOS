#include "disk.h"
#include "io.h"

#define ATA_DATA        0x170
#define ATA_ERROR       0x171
#define ATA_SECCOUNT    0x172
#define ATA_LBA_LO      0x173
#define ATA_LBA_MID     0x174
#define ATA_LBA_HI      0x175
#define ATA_DRIVE_HEAD  0x176
#define ATA_STATUS      0x177
#define ATA_COMMAND     0x177

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_IDENTIFY 0xEC

uint32_t disk_total_sectors = 0;

/* bounded busy-wait so a missing/broken disk can't hang the whole OS */
static int ata_wait_ready(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_SR_BSY)) {
            if (status & ATA_SR_ERR) return 0;
            return 1;
        }
    }
    return 0; /* timed out - treat as "no disk" */
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & ATA_SR_ERR) return 0;
        if (status & ATA_SR_DRQ) return 1;
    }
    return 0;
}

/* reads one 512-byte sector at LBA into buf. returns 1 on success. */
int ata_read_sector(uint32_t lba, uint8_t* buf) {
    if (!ata_wait_ready()) return 0;
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t) (lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t) ((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t) ((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_READ);
    if (!ata_wait_ready()) return 0;
    if (!ata_wait_drq()) return 0;
    for (int i = 0; i < 256; i++) {
        uint16_t w = inw(ATA_DATA);
        buf[i * 2] = (uint8_t) (w & 0xFF);
        buf[i * 2 + 1] = (uint8_t) ((w >> 8) & 0xFF);
    }
    return 1;
}

/* writes one 512-byte sector at LBA from buf. returns 1 on success. */
int ata_write_sector(uint32_t lba, const uint8_t* buf) {
    if (!ata_wait_ready()) return 0;
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t) (lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t) ((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t) ((lba >> 16) & 0xFF));
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (!ata_wait_ready()) return 0;
    if (!ata_wait_drq()) return 0;
    for (int i = 0; i < 256; i++) {
        uint16_t w = (uint16_t) buf[i * 2] | ((uint16_t) buf[i * 2 + 1] << 8);
        outw(ATA_DATA, w);
    }
    return 1;
}

/* returns 1 and fills disk_total_sectors if a plain ATA disk answered, 0 if
   there's nothing attached on this channel (or it's not a plain ATA disk -
   e.g. an ATAPI drive, which reports differently and isn't what we want here) */
int ata_identify(void) {
    if (!ata_wait_ready()) return 0;
    outb(ATA_DRIVE_HEAD, 0xE0);
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    if (inb(ATA_STATUS) == 0) return 0; /* no drive at all on this channel */
    if (!ata_wait_ready()) return 0;
    if (inb(ATA_LBA_MID) != 0 || inb(ATA_LBA_HI) != 0) return 0; /* not a plain ATA disk */
    if (!ata_wait_drq()) return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ATA_DATA);

    /* words 60-61: total addressable sectors in LBA28 mode, little-endian */
    disk_total_sectors = (uint32_t) id[60] | ((uint32_t) id[61] << 16);
    return disk_total_sectors > 0;
}

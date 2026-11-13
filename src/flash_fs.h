/*
 * flash_fs.h
 *
 * Flash layout of PicoBoyGB for the original PicoBoy Color (RP2040, 2 MB)
 * and the low-level block device that backs both FatFs (diskio.c) and the
 * USB mass-storage export (usb_msc.c).
 *
 * 2 MB flash map (offsets from XIP_BASE / flash start):
 *
 *   0x000000 .. 0x03FFFF   (256 KB)   firmware (size checked at build time)
 *   0x040000 .. 0x1EFFFF   (1.69 MB)  FatFs partition (this file) -> USB stick
 *   0x1F0000 .. 0x1FFFFF   (64 KB)    save region (main.cpp: SAVE_FLASH_OFFSET)
 *
 * There is no separate ROM region as on the PicoBoy Color Plus: a 1 MB game
 * (Pokemon) would not fit next to the drive. The emulator reads the ROM
 * straight from its file in the FatFs partition through XIP; main.cpp makes
 * sure the file occupies consecutive sectors.
 *
 * One FatFs sector == one 4096-byte flash sector, so a sector write maps
 * directly to flash_range_erase(4096) + flash_range_program(4096) with no
 * read-modify-write, and a file sector's flash address is simply
 * XIP_BASE + PBGB_FS_FLASH_OFFSET + lba * 4096.
 */

#ifndef PBGB_FLASH_FS_H
#define PBGB_FLASH_FS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PBGB_FS_FLASH_OFFSET   0x040000u                /* 256 KB */
#define PBGB_FS_FLASH_SIZE     0x1B0000u                /* 1728 KB */
#define PBGB_FS_SECTOR_SIZE    4096u                    /* == flash erase unit */
#define PBGB_FS_SECTOR_COUNT   (PBGB_FS_FLASH_SIZE / PBGB_FS_SECTOR_SIZE)  /* 432 */

#define PBGB_SAVE_FLASH_OFFSET 0x1F0000u                /* last 64 KB */
#define PBGB_SAVE_FLASH_SIZE   0x010000u

/* Raw block access used by diskio.c and the USB MSC callbacks.
 * lba is a sector index within the FS region (0 .. PBGB_FS_SECTOR_COUNT-1).
 * Both operate on whole PBGB_FS_SECTOR_SIZE sectors. Return 0 on success. */
int  flash_fs_read (uint32_t lba, void *dst, uint32_t sector_count);
int  flash_fs_write(uint32_t lba, const void *src, uint32_t sector_count);

#ifdef __cplusplus
}
#endif

#endif /* PBGB_FLASH_FS_H */

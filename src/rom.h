/*
 * rom.h
 *
 * ROM access for the RP2040 build.
 *
 * The ROM is not copied anywhere: the emulator reads it straight from its
 * .gb file in the FatFs partition through XIP (see flash_fs.h). main.cpp
 * looks up where the file's (consecutive) sectors are and points
 * active_rom at them.
 */

#ifndef PICOBOY_ROM_H
#define PICOBOY_ROM_H

#include <stdint.h>
#include <stddef.h>

// The largest DMG cartridges have 1 MB (8 Mbit), e.g. Pokemon Red/Blue/Yellow.
#define ROM_MAX_SIZE       (1u * 1024u * 1024u)

// The first 16 bytes of the Nintendo logo (at 0x104 in every valid GB ROM).
static const uint8_t kNintendoLogoHead[16] = {
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B,
    0x03, 0x73, 0x00, 0x83, 0x00, 0x0C, 0x00, 0x0D
};

static inline bool rom_looks_valid(const uint8_t *rom) {
    for (int i = 0; i < 16; ++i) {
        if (rom[0x104 + i] != kNintendoLogoHead[i]) return false;
    }
    return true;
}

#endif // PICOBOY_ROM_H

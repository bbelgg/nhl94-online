/* Minimal consumer-owned ROM interface for the synthetic codegen-flow test. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GENESIS_ROM_MAX_SIZE 0x800000

typedef struct {
    uint8_t  *rom_data;
    uint32_t  rom_size;
    uint8_t (*read8_override)(uint32_t addr, void *user);
    void     *read8_user;
} GenesisRom;

uint16_t rom_read16(const GenesisRom *rom, uint32_t addr);
uint32_t rom_read32(const GenesisRom *rom, uint32_t addr);

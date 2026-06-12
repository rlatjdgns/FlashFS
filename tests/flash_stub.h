#pragma once
#include <cstdint>
#include <cstring>
#include "fs.h"   // FLASH_SECTOR_SIZE, FLASH_TOTAL_SECTORS

// ---- RAM-backed flash stub. EN25Q64 is 8 MiB; size up for headroom. ----
// Shared by the FS unit test and the stress test (included in exactly one TU
// per binary). fs.cpp links against the three flash_* functions.
static uint8_t flash_mem[9 * 1024 * 1024];

// Cumulative PHYSICAL erase count per sector. Survives flash_reset() so the
// stress test can measure true wear across reformats.
static uint32_t g_phys_erase[FLASH_TOTAL_SECTORS];

static void flash_reset() { memset(flash_mem, 0xFF, sizeof(flash_mem)); }

void flash_sector_erase(uint32_t address) {
    uint32_t base = address & ~((uint32_t)(FLASH_SECTOR_SIZE - 1));
    memset(&flash_mem[base], 0xFF, FLASH_SECTOR_SIZE);
    uint32_t idx = base / FLASH_SECTOR_SIZE;
    if (idx < FLASH_TOTAL_SECTORS) g_phys_erase[idx]++;
}
void flash_page_program(uint32_t address, uint8_t* data, uint32_t length) {
    // NOR flash programs 1->0 only; sectors are erased (0xFF) before programming.
    for (uint32_t i = 0; i < length; i++) flash_mem[address + i] &= data[i];
}
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) buffer[i] = flash_mem[address + i];
}

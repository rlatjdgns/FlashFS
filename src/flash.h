#pragma once
#include <stdint.h>

void flash_read_jedec_id();
void flash_write_enable();
void flash_wait_busy();
void flash_sector_erase(uint32_t address);
void flash_page_program(uint32_t address, uint8_t* data, uint32_t length);
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length);
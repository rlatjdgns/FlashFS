#pragma once
#include <stdint.h>

// Page states
#define PAGE_ERASED  0xFF
#define PAGE_VALID   0xFE
#define PAGE_INVALID 0xFC

// Directory entry states
#define FILE_ACTIVE  0x01
#define FILE_DELETED 0x00

// EN25Q64 geometry
#define FLASH_TOTAL_SECTORS  2048
#define FLASH_SECTOR_SIZE    4096
#define FLASH_PAGE_SIZE      256
#define FLASH_DATA_SIZE      244   // 256 - sizeof(PageHeader); PageHeader is 12 bytes (with padding)

// Fixed addresses
#define SUPERBLOCK_ADDR      0x000000
#define ALLOC_TABLE_ADDR     0x001000  // sector 1
#define DATA_START_ADDR      0x004000  // sector 4

// Allocation table sector states
#define SECTOR_FREE    0x00
#define SECTOR_ACTIVE  0x01
#define SECTOR_FULL    0x02

#define MAX_FILES 32

// On-flash layout version. BUMP THIS whenever any on-flash struct size/layout
// changes (e.g. AllocationEntry packing) so fs_init force-reformats stale flash
// instead of misreading an old layout.
#define FS_FORMAT_VERSION 2

struct PageHeader {
    uint8_t  state;
    uint8_t  file_id;
    uint32_t file_offset;
    uint8_t  data_length;
    uint16_t crc;
};

struct DirectoryEntry{
    char filename[16];
    uint8_t file_id;
    uint32_t file_size;
    uint32_t firstpage_addr;
    uint32_t created; 
    uint8_t status;
};

// Packed to 5 bytes so the allocation table fits in the 3 sectors reserved
// between ALLOC_TABLE_ADDR (0x001000) and DATA_START_ADDR (0x004000).
// Unpacked it pads to 8 bytes -> table needs 4 sectors and the 4th would
// overwrite data sector 0.
struct AllocationEntry {
    uint8_t  state;
    uint32_t erase_count;
} __attribute__((packed));

struct Superblock{
    uint32_t magic;
    uint8_t version;
    uint16_t total_sectors;
    uint16_t sector_size;
    uint16_t page_size;
    uint8_t num_files;
    uint32_t alloc_table_addr;
};

int  fs_init();
int  fs_create(const char* filename);
int  fs_write(uint8_t file_id, uint8_t * data, uint32_t length);
int  fs_read(uint8_t file_id, uint8_t * buffer, uint32_t length);
uint32_t fs_find_free_sector();

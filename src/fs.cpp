#include "fs.h"
#include "flash.h"

static uint8_t page_buf[FLASH_PAGE_SIZE];
static uint8_t sector_buf[FLASH_SECTOR_SIZE];
static uint32_t last_written_sector = 0;

static uint16_t crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}


int fs_init(){
    //Read superblock and check magic number to determine if flash is already formatted
    Superblock sb;
    flash_read_data(SUPERBLOCK_ADDR, (uint8_t*)&sb, sizeof(Superblock));

    if(sb.magic == 0xDEADBEEF){
        //Flash already formatted — validate geometry matches compile-time constants
        if(sb.total_sectors != FLASH_TOTAL_SECTORS ||
           sb.sector_size   != FLASH_SECTOR_SIZE   ||
           sb.page_size     != FLASH_PAGE_SIZE)
            return -1;
        return 0;
    }

    //Magic not found — format the flash from scratch
    Superblock fresh;
    fresh.magic = 0xDEADBEEF;
    fresh.version = 1;
    fresh.total_sectors = FLASH_TOTAL_SECTORS;
    fresh.sector_size = FLASH_SECTOR_SIZE;
    fresh.page_size = FLASH_PAGE_SIZE;
    fresh.num_files = 0;
    fresh.alloc_table_addr = ALLOC_TABLE_ADDR;

    //Write superblock to sector 0
    flash_sector_erase(SUPERBLOCK_ADDR);
    flash_page_program(SUPERBLOCK_ADDR, (uint8_t*)&fresh, sizeof(Superblock));

    //Zero-initialize the allocation table so all sectors start as SECTOR_FREE
    for(uint32_t k = 0; k < sizeof(sector_buf); k++)
        sector_buf[k] = 0x00;
    
    //Calculate how many flash sectors the allocation table needs -> 3 
        uint32_t alloc_sectors = (FLASH_TOTAL_SECTORS * sizeof(AllocationEntry) + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    
    //computing the flash address of each allocation table sectors     
    for(uint32_t s = 0; s < alloc_sectors; s++){
        uint32_t addr = ALLOC_TABLE_ADDR + s * FLASH_SECTOR_SIZE;
        flash_sector_erase(addr);
        //Write the all-zeros sector_buf to every page in that sector
        for(uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE)
            flash_page_program(addr + p, sector_buf, FLASH_PAGE_SIZE);
    }

    return 0;
}

//Return the data sector with the lowest erase count (wear leveling)
uint32_t fs_find_free_sector(){
    uint32_t lowest_erase_count = UINT32_MAX;
    uint32_t best_sector = 0xFFFFFFFF;
    uint32_t cached_alloc_base = 0xFFFFFFFF;

    //i is the data sector index (0 = first data sector); allocation table index is i+4
    for(uint16_t i = 0; i < FLASH_TOTAL_SECTORS - 4; i++){
        uint32_t alloc_addr = ALLOC_TABLE_ADDR + (i + 4) * sizeof(AllocationEntry);
        uint32_t alloc_sector_base = alloc_addr & ~((uint32_t)(FLASH_SECTOR_SIZE - 1));

        //Reload sector_buf only when crossing into a new allocation table sector
        if(alloc_sector_base != cached_alloc_base){
            flash_read_data(alloc_sector_base, sector_buf, FLASH_SECTOR_SIZE);
            cached_alloc_base = alloc_sector_base;
        }

        //Read entry from buffer; fall back to direct read if it straddles a sector boundary
        AllocationEntry entry;
        uint32_t entry_offset = alloc_addr - alloc_sector_base;
        if(entry_offset + sizeof(AllocationEntry) <= FLASH_SECTOR_SIZE){
            for(uint32_t k = 0; k < sizeof(AllocationEntry); k++)
                ((uint8_t*)&entry)[k] = sector_buf[entry_offset + k];
        } else {
            flash_read_data(alloc_addr, (uint8_t*)&entry, sizeof(AllocationEntry));
        }

        //Skip 0xFF — uninitialized flash, not a valid free entry
        if(entry.state == SECTOR_FREE){
            if(entry.erase_count < lowest_erase_count){
                lowest_erase_count = entry.erase_count;
                best_sector = DATA_START_ADDR + i * FLASH_SECTOR_SIZE;
            }
        }
    }
    return best_sector;
}

//Finds an empty directory slot and writes a new DirectoryEntry into it
int fs_create(const char* filename){
    for(uint8_t i = 0; i < MAX_FILES; i++){
        //Read existing slot to check if it is free
        DirectoryEntry entry;
        uint32_t addr = SUPERBLOCK_ADDR + sizeof(Superblock) + i * sizeof(DirectoryEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));

        if(entry.status == FILE_DELETED || entry.status == 0xFF){
            //Build new entry in RAM
            for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++)
                ((uint8_t*)&entry)[k] = 0;
            entry.status = FILE_ACTIVE;
            entry.file_id = i;

            //Copy filename safely: max 15 chars + null terminator at [15]
            for(int j = 0; j < 15; j++){
                entry.filename[j] = filename[j];
                if(filename[j] == '\0') break;
            }
            entry.filename[15] = '\0';

            //Read-modify-erase-rewrite sector 0 to update directory entry and num_files
            flash_read_data(SUPERBLOCK_ADDR, sector_buf, FLASH_SECTOR_SIZE);
            DirectoryEntry* e = (DirectoryEntry*)(sector_buf + sizeof(Superblock) + i * sizeof(DirectoryEntry));
            for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++)
                ((uint8_t*)e)[k] = ((uint8_t*)&entry)[k];
            Superblock* sb = (Superblock*)sector_buf;
            sb->num_files += 1;
            flash_sector_erase(SUPERBLOCK_ADDR);
            for(uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE)
                flash_page_program(SUPERBLOCK_ADDR + p, sector_buf + p, FLASH_PAGE_SIZE);

            return i;
        }
    }
    //All slots occupied
    return -1;
}

int fs_write(uint8_t file_id, uint8_t* data, uint32_t length){
    //Locate free sector; return early if flash is full
    uint32_t free_sector = fs_find_free_sector();
    if(free_sector == 0xFFFFFFFF)
        return -1;
    last_written_sector = free_sector;

    //Erase sector before writing so all bits start at 1
    flash_sector_erase(free_sector);

    //Set up write loop cursors
    uint32_t bytes_remaining = length;
    uint32_t offset = 0;
    uint32_t page_addr = free_sector;

    //Write data page by page
    while(bytes_remaining > 0){
        //min(bytes_remaining, FLASH_DATA_SIZE) without using stdlib
        uint32_t chunk_size = bytes_remaining < (uint32_t)FLASH_DATA_SIZE ? bytes_remaining : (uint32_t)FLASH_DATA_SIZE;

        //Copy payload into buffer first so CRC can be computed over it
        for(uint32_t i = 0; i < chunk_size; i++)
            page_buf[sizeof(PageHeader) + i] = data[offset + i];

        //Build page header with CRC over the payload
        PageHeader ph;
        ph.state = PAGE_VALID;
        ph.file_id = file_id;
        ph.file_offset = offset;
        ph.data_length = chunk_size;
        ph.crc = crc16(page_buf + sizeof(PageHeader), chunk_size);

        //Copy header into front of buffer
        for(uint32_t i = 0; i < sizeof(PageHeader); i++)
            page_buf[i] = ((uint8_t*)&ph)[i];

        flash_page_program(page_addr, page_buf, FLASH_PAGE_SIZE);
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        page_addr += FLASH_PAGE_SIZE;
    }

    //Scan directory to find the matching entry and update it
    for(int i = 0; i < MAX_FILES; i++){
        DirectoryEntry entry;
        uint32_t addr = SUPERBLOCK_ADDR + sizeof(Superblock) + i * sizeof(DirectoryEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));
        if(entry.file_id == file_id && entry.status == FILE_ACTIVE){
            //Read-modify-erase-rewrite sector 0 (Superblock + directory)
            flash_read_data(SUPERBLOCK_ADDR, sector_buf, FLASH_SECTOR_SIZE);
            DirectoryEntry* e = (DirectoryEntry*)(sector_buf + sizeof(Superblock) + i * sizeof(DirectoryEntry));
            e->firstpage_addr = free_sector;
            e->file_size = length;
            flash_sector_erase(SUPERBLOCK_ADDR);
            for(uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE){
                flash_page_program(SUPERBLOCK_ADDR + p, sector_buf + p, FLASH_PAGE_SIZE);
            }

            //Read-modify-erase-rewrite allocation table sector
            uint32_t sector_index = (free_sector - DATA_START_ADDR) / FLASH_SECTOR_SIZE + 4;
            uint32_t alloc_addr = ALLOC_TABLE_ADDR + sector_index * sizeof(AllocationEntry);
            uint32_t alloc_sector_base = alloc_addr & ~((uint32_t)(FLASH_SECTOR_SIZE - 1));
            flash_read_data(alloc_sector_base, sector_buf, FLASH_SECTOR_SIZE);
            AllocationEntry* ae = (AllocationEntry*)(sector_buf + (alloc_addr - alloc_sector_base));
            ae->state = SECTOR_ACTIVE;
            ae->erase_count += 1;
            flash_sector_erase(alloc_sector_base);
            for(uint32_t p = 0; p < FLASH_SECTOR_SIZE; p += FLASH_PAGE_SIZE){
                flash_page_program(alloc_sector_base + p, sector_buf + p, FLASH_PAGE_SIZE);
            }
            return 0;
        }
    }
    return -1;
}

int fs_read(uint8_t file_id, uint8_t* buffer, uint32_t length){
    //Look up file's starting address from the directory
    uint32_t start_addr = 0xFFFFFFFF;
    for(int i = 0; i < MAX_FILES; i++){
        DirectoryEntry entry;
        uint32_t addr = SUPERBLOCK_ADDR + sizeof(Superblock) + i * sizeof(DirectoryEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));
        if(entry.file_id == file_id && entry.status == FILE_ACTIVE){
            start_addr = entry.firstpage_addr;
            break;
        }
    }
    if(start_addr == 0xFFFFFFFF)
        return -1;

    //Scan all pages in the sector and reassemble the file
    uint32_t page_addr = start_addr;
    uint32_t end_addr = start_addr + FLASH_SECTOR_SIZE;

    while(page_addr < end_addr){
        flash_read_data(page_addr, page_buf, FLASH_PAGE_SIZE);

        //Extract page header from front of buffer
        PageHeader ph;
        for(uint32_t j = 0; j < sizeof(PageHeader); j++)
            ((uint8_t*)&ph)[j] = page_buf[j];

        //Stop early if we hit an erased page — no more data beyond this point
        if(ph.state == PAGE_ERASED)
            break;

        if(ph.file_id == file_id && ph.state == PAGE_VALID){
            //Verify CRC before trusting the payload
            uint16_t computed = crc16(page_buf + sizeof(PageHeader), ph.data_length);
            if(computed != ph.crc)
                return -1;

            //Copy payload into caller's buffer at the correct file offset
            for(uint32_t j = 0; j < ph.data_length; j++){
                if(ph.file_offset + j < length)
                    buffer[ph.file_offset + j] = page_buf[sizeof(PageHeader) + j];
            }
        }
        page_addr += FLASH_PAGE_SIZE;
    }
    return 0;
}

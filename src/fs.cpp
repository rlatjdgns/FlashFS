#include "fs.h"
#include "flash.h"

void uart_send_byte(uint8_t data);
void uart_send_string(const char* s);
static uint8_t page_buf[256];
static uint32_t last_written_sector = 0;


void fs_init(){
    Superblock sb;
    flash_read_data(SUPERBLOCK_ADDR, (uint8_t*)&sb, sizeof(Superblock));
    if(sb.magic==0xDEADBEEF){

    }
    else{
        Superblock fresh;
        fresh.magic = 0xDEADBEEF;
        fresh.version = 1;
        fresh.total_sectors = FLASH_TOTAL_SECTORS;
        fresh.sector_size = FLASH_SECTOR_SIZE;
        fresh.page_size = FLASH_PAGE_SIZE;
        fresh.num_files = 0;
        fresh.alloc_table_addr = ALLOC_TABLE_ADDR;

        flash_sector_erase(SUPERBLOCK_ADDR);
        flash_page_program(SUPERBLOCK_ADDR, (uint8_t*)&fresh, sizeof(Superblock));
    }
}

//Return the sector with lowest erase count 
    uint32_t fs_find_free_sector(){
    uint32_t lowest_erase_count = UINT32_MAX;
    uint32_t best_sector = 0xFFFFFFFF; 
    
    for(uint16_t i = 4; i < FLASH_TOTAL_SECTORS; i++){
        AllocationEntry entry;
        uint32_t addr = ALLOC_TABLE_ADDR + i * sizeof(AllocationEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(AllocationEntry));
        
        if(entry.state == SECTOR_FREE || entry.state == 0xFF){
            if(entry.erase_count <= lowest_erase_count){
                lowest_erase_count = entry.erase_count;
                best_sector = DATA_START_ADDR + (i - 4) * FLASH_SECTOR_SIZE;
            }
        }
    }
    return best_sector;
}

//
void fs_create(const char*filename){
    for(uint8_t i=0; i<MAX_FILES;i++){
        DirectoryEntry entry;
        for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++){
            ((uint8_t*)&entry)[k] = 0;
        } 
        uint32_t addr = SUPERBLOCK_ADDR + sizeof(Superblock) + i * sizeof(DirectoryEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));

        if(entry.status==FILE_DELETED||entry.status== 0xFF){
            entry.status = FILE_ACTIVE;
            entry.file_id = i;
            for(int j = 0; j < 16; j++){
                entry.filename[j] = filename[j];
            }
            flash_page_program(addr,(uint8_t*)&entry, sizeof(DirectoryEntry));
            return;
        }
    }
}

void fs_write(uint8_t file_id, uint8_t* data, uint32_t length){
    uint32_t free_sector = fs_find_free_sector();
    last_written_sector = free_sector;
    uart_send_string("free_sector: ");
    uart_send_byte((free_sector >> 24) & 0xFF);
    uart_send_byte((free_sector >> 16) & 0xFF);
    uart_send_byte((free_sector >> 8) & 0xFF);
    uart_send_byte((free_sector >> 0) & 0xFF);
    uart_send_byte('\n');

    uint32_t bytes_remaining = length;
    uint32_t offset = 0;
    uint32_t page_addr = free_sector;

    while(bytes_remaining>0){
        uint32_t chunk_size = bytes_remaining < (uint32_t)FLASH_DATA_SIZE ? bytes_remaining : (uint32_t)FLASH_DATA_SIZE;
        PageHeader ph; 

        ph.state = PAGE_VALID;
        ph.file_id = file_id;
        ph.file_offset = offset;
        ph.data_length = chunk_size;
        ph.crc = 0;

        // copy header into buffer
        for(uint32_t i = 0; i < sizeof(PageHeader); i++){
            page_buf[i] = ((uint8_t*)&ph)[i];
        }

        // copy data into buffer after header
        for(uint32_t i = 0; i < chunk_size; i++){
            page_buf[sizeof(PageHeader) + i] = data[offset + i];
        }

        flash_page_program(page_addr, page_buf, FLASH_PAGE_SIZE);
        offset += chunk_size;
        bytes_remaining -= chunk_size;
        page_addr += FLASH_PAGE_SIZE;
    }
    for(int i=0; i<MAX_FILES;i++){
        DirectoryEntry entry;
        for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++){
            ((uint8_t*)&entry)[k] = 0;
        }
        uint32_t addr = SUPERBLOCK_ADDR + sizeof(Superblock) + i * sizeof(DirectoryEntry);
        flash_read_data(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));
        if(entry.file_id==file_id){
            uart_send_string("Found entry\n");
            uart_send_byte(entry.file_id);
            uart_send_byte(entry.status);    
            entry.firstpage_addr = free_sector;
            entry.file_size = length;
            flash_page_program(addr, (uint8_t*)&entry, sizeof(DirectoryEntry));
            // Update allocation table
            uint32_t sector_index = (free_sector - DATA_START_ADDR) / FLASH_SECTOR_SIZE + 4;
            AllocationEntry alloc;
            uint32_t alloc_addr = ALLOC_TABLE_ADDR + sector_index * sizeof(AllocationEntry);
            flash_read_data(alloc_addr, (uint8_t*)&alloc, sizeof(AllocationEntry));
            alloc.state = SECTOR_ACTIVE;
            alloc.erase_count += 1;
            flash_page_program(alloc_addr, (uint8_t*)&alloc, sizeof(AllocationEntry));
            return;
        }
    }
  
}

void fs_read(uint8_t file_id, uint8_t* buffer, uint32_t length){
    uint32_t page_addr = last_written_sector;
    uint32_t end_addr = last_written_sector + FLASH_SECTOR_SIZE;
    
    while(page_addr < end_addr){
        uint8_t page_buf[FLASH_PAGE_SIZE];
        flash_read_data(page_addr, page_buf, FLASH_PAGE_SIZE);
        
        PageHeader ph;
        for(uint32_t j = 0; j < sizeof(PageHeader); j++){
            ((uint8_t*)&ph)[j] = page_buf[j];
        }
        
        if(ph.file_id == file_id && ph.state == PAGE_VALID){
            for(uint32_t j = 0; j < ph.data_length; j++){
                if(ph.file_offset + j < length){
                    buffer[ph.file_offset + j] = page_buf[sizeof(PageHeader) + j];
                }
            }
        }
        page_addr += FLASH_PAGE_SIZE;
    }
}
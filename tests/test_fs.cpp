#include <cstdint>
#include <cstring>
#include <cstdio>
#include "fs.h"
#include "flash.h"
#include "demo.h"   // SensorReading { uint32_t seq; int32_t temp; }

// ---- RAM-backed flash stub. W25Q64 is 8 MiB; size up for headroom. ----
// fs.cpp calls only these three flash functions.
static uint8_t flash_mem[9 * 1024 * 1024];

static void flash_reset() { memset(flash_mem, 0xFF, sizeof(flash_mem)); }

void flash_sector_erase(uint32_t address) {
    uint32_t base = address & ~((uint32_t)(FLASH_SECTOR_SIZE - 1));
    memset(&flash_mem[base], 0xFF, FLASH_SECTOR_SIZE);
}
void flash_page_program(uint32_t address, uint8_t* data, uint32_t length) {
    // NOR flash programs 1->0 only; sectors are erased (0xFF) before programming.
    for (uint32_t i = 0; i < length; i++) flash_mem[address + i] &= data[i];
}
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length) {
    for (uint32_t i = 0; i < length; i++) buffer[i] = flash_mem[address + i];
}

// ---- Tiny assertion harness ----
static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_failures++; } \
} while (0)

// A file that exists in the directory but was never written must report failure
// on read, not silently leave the caller's buffer untouched.
static void test_unwritten_file_returns_error() {
    flash_reset();
    fs_init();
    int id = fs_create("s00");
    CHECK(id == 0);

    SensorReading r; r.seq = 123; r.temp = 456;   // pre-fill so a "success" with no copy is detectable
    int rc = fs_read((uint8_t)id, (uint8_t*)&r, sizeof(r));
    CHECK(rc == -1);
}

int main() {
    test_unwritten_file_returns_error();
    if (g_failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
}

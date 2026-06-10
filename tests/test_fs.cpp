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

// A non-zero file_id round-trips once the file has been created.
static void test_roundtrip_nonzero_fileid() {
    flash_reset();
    fs_init();
    fs_create("s00");             // id 0
    int id = fs_create("s01");    // id 1
    CHECK(id == 1);

    SensorReading w; w.seq = 42; w.temp = 2805;
    CHECK(fs_write((uint8_t)id, (uint8_t*)&w, sizeof(w)) == 0);

    SensorReading r; r.seq = 0; r.temp = 0;
    CHECK(fs_read((uint8_t)id, (uint8_t*)&r, sizeof(r)) == 0);
    CHECK(r.seq == 42 && r.temp == 2805);
}

// Data survives a simulated reboot: fs_init again without erasing flash, then read.
static void test_persistence_across_reinit() {
    flash_reset();
    fs_init();
    fs_create("s00");
    SensorReading w; w.seq = 7; w.temp = 2900;
    CHECK(fs_write(0, (uint8_t*)&w, sizeof(w)) == 0);

    // "reboot": re-init WITHOUT clearing flash_mem
    CHECK(fs_init() == 0);          // already formatted -> no reformat
    CHECK(fs_create("s00") == 0);   // idempotent by name -> same id

    SensorReading r; r.seq = 0; r.temp = 0;
    CHECK(fs_read(0, (uint8_t*)&r, sizeof(r)) == 0);
    CHECK(r.seq == 7 && r.temp == 2900);
}

// Creating files in order assigns file_id == creation index; 33rd has no slot.
static void test_create_assigns_sequential_ids() {
    flash_reset();
    fs_init();
    char name[4]; name[0] = 's';
    for (uint8_t i = 0; i < MAX_FILES; i++) {
        name[1] = '0' + (i / 10);
        name[2] = '0' + (i % 10);
        name[3] = '\0';
        CHECK(fs_create(name) == (int)i);
    }
    CHECK(fs_create("zz") == -1);   // all MAX_FILES slots occupied
}

int main() {
    test_unwritten_file_returns_error();
    test_roundtrip_nonzero_fileid();
    test_persistence_across_reinit();
    test_create_assigns_sequential_ids();
    if (g_failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "fs.h"
#include "flash.h"
#include "flash_stub.h"

static const int WLEN = 16;

static void make_data(uint64_t seq, uint8_t* d) {
    for (int i = 0; i < WLEN; i++) d[i] = (uint8_t)(seq * 31 + i);
}

static void create_files() {
    char name[4]; name[0] = 's';
    for (uint8_t i = 0; i < MAX_FILES; i++) {
        name[1] = '0' + (i / 10); name[2] = '0' + (i % 10); name[3] = '\0';
        fs_create(name);
    }
}

static void reformat() {
    flash_reset();
    fs_init();
    create_files();
}

int main(int argc, char** argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 500;

    long total_writes = 0, total_reads = 0, mismatches = 0;
    uint64_t seq = 0;
    uint8_t data[WLEN], rb[WLEN];

    // Phase 1: cyclic integrity (reformat on full).
    reformat();
    for (int cycle = 0; cycle < N; cycle++) {
        for (uint8_t slot = 0; slot < MAX_FILES; slot++) {
            make_data(seq, data);
            if (fs_write(slot, data, WLEN) != 0) {   // flash full -> reformat + retry
                reformat();
                fs_write(slot, data, WLEN);
            }
            total_writes++;
            memset(rb, 0, WLEN);
            if (fs_read(slot, rb, WLEN) != 0 || memcmp(rb, data, WLEN) != 0)
                mismatches++;
            total_reads++;
            seq++;
        }
        if ((cycle + 1) % 50 == 0)
            printf("[stress] cycle %d/%d  writes=%ld mismatches=%ld\n",
                   cycle + 1, N, total_writes, mismatches);
    }

    // Phase 2: fill-to-capacity.
    reformat();
    long cap = 0;
    for (;;) {
        make_data(cap, data);
        if (fs_write((uint8_t)(cap % MAX_FILES), data, WLEN) != 0) break;
        cap++;
    }
    int phase2_bad = 0;
    for (uint8_t slot = 0; slot < MAX_FILES; slot++)
        if (fs_read(slot, rb, WLEN) != 0) phase2_bad++;

    // Cumulative erase stats over data sectors (physical sector index 4..2047).
    uint32_t emin = 0xFFFFFFFF, emax = 0; double esum = 0; int ecount = 0;
    for (uint32_t i = 4; i < FLASH_TOTAL_SECTORS; i++) {
        uint32_t e = g_phys_erase[i];
        if (e < emin) emin = e;
        if (e > emax) emax = e;
        esum += e; ecount++;
    }

    // Save wear as an allocation-table-layout blob (data sectors only).
    {
        static uint8_t blob[3 * FLASH_SECTOR_SIZE];
        memset(blob, 0, sizeof(blob));
        for (uint32_t j = 0; j < FLASH_TOTAL_SECTORS; j++) {
            uint8_t state = SECTOR_FREE; uint32_t erase = 0;
            if (j >= 4) { erase = g_phys_erase[j]; state = erase ? SECTOR_ACTIVE : SECTOR_FREE; }
            blob[j * 5] = state;
            memcpy(&blob[j * 5 + 1], &erase, 4);
        }
        FILE* f = fopen("tools/stress_result.bin", "wb");
        if (f) { fwrite(blob, 1, sizeof(blob), f); fclose(f); }
    }

    printf("\nSUMMARY:\n");
    printf("  total_writes: %ld\n", total_writes);
    printf("  total_reads:  %ld\n", total_reads);
    printf("  mismatches:   %ld\n", mismatches);
    printf("  max_capacity_writes: %ld\n", cap);
    printf("  phase2_unreadable_slots: %d\n", phase2_bad);
    printf("  erase_count(data sectors) min=%u max=%u avg=%.2f\n",
           emin == 0xFFFFFFFF ? 0 : emin, emax, ecount ? esum / ecount : 0.0);
    printf("  saved tools/stress_result.bin\n");

    return (mismatches > 0 || phase2_bad > 0) ? 1 : 0;
}

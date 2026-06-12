# Flash FS Stress Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A host-side C++ stress test that hammers the FS over 500×32 writes (reformatting on full), verifies every read matches its write, characterizes write capacity, and emits cumulative wear to a binary the analyzer can chart.

**Architecture:** Extract the existing RAM flash stub to a shared header and add a cumulative physical-erase counter that survives `flash_reset`. The stress binary drives `fs_write`/`fs_read`, reformats on flash-full, and writes the wear as an allocation-table blob; `flash_analyzer.py --file` charts it. No FS logic changes.

**Tech Stack:** C++ (host `c++`/clang), Python 3 (analyzer + CI wrapper).

**Design spec:** `docs/superpowers/specs/2026-06-12-fs-stress-test-design.md`

**Constraint:** Touch ONLY `tests/flash_stub.h`, `tests/test_fs.cpp`, `tests/stress_test.cpp`, `tests/test_stress.py`, `tools/flash_analyzer.py`, `Makefile`. Do NOT change `fs.cpp`/`fs.h`/`flash.cpp`. `stress_result.bin` is covered by `*.bin` in `.gitignore`.

---

## Pre-flight

```bash
git checkout -b feat-fs-stress-test
```

---

### Task 1: Extract the flash stub to a shared, instrumented header

**Files:**
- Create: `tests/flash_stub.h`
- Modify: `tests/test_fs.cpp`

- [ ] **Step 1: Create `tests/flash_stub.h`**

```cpp
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
```

- [ ] **Step 2: Replace the inline stub in `tests/test_fs.cpp` with the include**

In `tests/test_fs.cpp`, delete this block:

```cpp
// ---- RAM-backed flash stub. EN25Q64 is 8 MiB; size up for headroom. ----
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
```

and replace it with a single line:

```cpp
#include "flash_stub.h"
```

- [ ] **Step 3: Verify the FS unit tests still pass (regression)**

Run: `make test`
Expected: `ALL TESTS PASSED` (the stub move changed nothing observable).

- [ ] **Step 4: Commit**

```bash
git add tests/flash_stub.h tests/test_fs.cpp
git commit -m "refactor(tests): extract RAM flash stub to shared header + physical-erase counter"
```

---

### Task 2: Stress test binary + CI wrapper + make target

**Files:**
- Create: `tests/test_stress.py`
- Create: `tests/stress_test.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Write the failing CI test**

Create `tests/test_stress.py`:

```python
import subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
binp = "/tmp/stress_test_ci"

c = subprocess.run(["c++", "-std=c++17", "-I", ROOT + "/src", "-O0", "-o", binp,
                    ROOT + "/tests/stress_test.cpp", ROOT + "/src/fs.cpp"],
                   capture_output=True, text=True)
if c.returncode != 0:
    print(c.stderr)
    sys.exit("compile failed")

r = subprocess.run([binp, "50"], cwd=ROOT, capture_output=True, text=True)
print(r.stdout[-600:])

mism = None
for line in r.stdout.splitlines():
    if "mismatches:" in line:
        mism = int(line.split(":")[1].strip())

ok = (r.returncode == 0) and (mism == 0)
print("test_stress: PASS" if ok else "test_stress: FAIL")
sys.exit(0 if ok else 1)
```

- [ ] **Step 2: Run it to verify it FAILS**

Run: `python3 tests/test_stress.py`
Expected: compile error (`stress_test.cpp` does not exist) → `compile failed`, nonzero exit.

- [ ] **Step 3: Write `tests/stress_test.cpp`**

```cpp
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
```

- [ ] **Step 4: Add the `stress` target to `Makefile`**

Append to `Makefile` (after the existing `test` target):

```makefile

stress:
	mkdir -p build
	$(HOSTCXX) -std=c++17 -I src -O0 -g -o build/stress_test tests/stress_test.cpp src/fs.cpp
	./build/stress_test
```

- [ ] **Step 5: Run the CI test to verify it PASSES**

Run: `python3 tests/test_stress.py`
Expected: prints the SUMMARY with `mismatches:   0`, then `test_stress: PASS`, exit 0.

- [ ] **Step 6: Verify the full stress run + the FS unit test still passes**

Run: `make stress`
Expected: progress lines + SUMMARY with `mismatches:   0`, `max_capacity_writes` ≈ 2044, an erase avg in the single digits, `saved tools/stress_result.bin`.
Run: `make test`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 7: Commit**

```bash
git add tests/stress_test.cpp tests/test_stress.py Makefile
git commit -m "feat: host-side FS stress test (integrity + capacity + wear) with CI wrapper"
```

---

### Task 3: flash_analyzer `--file` mode

**Files:**
- Modify: `tools/flash_analyzer.py`

- [ ] **Step 1: Replace `main()` to dispatch on `--file`**

In `tools/flash_analyzer.py`, replace the entire `def main():` function with:

```python
def main():
    import sys
    args = sys.argv[1:]

    # Offline mode: chart a saved allocation-table blob (e.g. tools/stress_result.bin).
    if len(args) >= 2 and args[0] == "--file":
        with open(args[1], "rb") as f:
            alloc = f.read()
        entries = parse_alloc_table(alloc)
        active = [e for e in entries if e[1] == SECTOR_ACTIVE]
        er = [e[2] for e in active]
        print(f"ALLOC (from {args[1]}): active={len(active)} "
              f"erase min/max/avg={min(er) if er else 0}/{max(er) if er else 0}/"
              f"{(sum(er)/len(er)) if er else 0:.2f}")
        wear_chart(entries, "docs/wear-leveling.png")
        return

    # Live mode: trigger a board dump over UART.
    import serial
    if len(args) != 2:
        sys.exit("usage: python3 flash_analyzer.py <port> <baud>  |  --file <path>")
    ser = serial.Serial(args[0], int(args[1]), timeout=0.3)
    print("Press the board RESET button now...", file=sys.stderr)
    sector0, alloc, sectors = read_dump(ser)
    ser.close()

    sb = parse_superblock(sector0)
    print(f"\nSUPERBLOCK  magic={sb['magic']:#010x}  version={sb['version']}  "
          f"sectors={sb['total_sectors']}  sector={sb['sector_size']}B  page={sb['page_size']}B  "
          f"files={sb['num_files']}")

    print("\nDIRECTORY")
    print(f"  {'filename':16} {'id':>3} {'size':>8} {'firstpage':>10}")
    for e in parse_directory(sector0):
        print(f"  {e['filename']:16} {e['file_id']:>3} {e['file_size']:>8} {e['firstpage_addr']:#010x}")

    entries = parse_alloc_table(alloc)
    erases = [e for (_, s, e) in entries if s == SECTOR_ACTIVE]
    print("\nALLOCATION TABLE")
    print(f"  active sectors: {len(erases)}")
    if erases:
        print(f"  erase_count  min={min(erases)}  max={max(erases)}  avg={sum(erases)/len(erases):.1f}")

    print("\nDATA PAGES")
    for addr, data in sectors:
        for pg in parse_sector_pages(data):
            ok = "PASS" if pg["crc_ok"] else "FAIL"
            print(f"  sector {addr:#010x}  file_id={pg['file_id']}  off={pg['file_offset']}  "
                  f"len={pg['data_length']}  crc={pg['crc']:#06x}  {ok}")

    wear_chart(entries, "docs/wear-leveling.png")
```

- [ ] **Step 2: Verify it compiles and the parser tests still pass**

Run: `python3 -m py_compile tools/flash_analyzer.py && echo OK`
Run: `python3 tests/test_analyzer.py | tail -1`
Expected: `OK` then `ALL TESTS PASSED` (parsers untouched).

- [ ] **Step 3: Verify `--file` mode against the real stress output**

Run: `make stress` (regenerates `tools/stress_result.bin`), then
`python3 tools/flash_analyzer.py --file tools/stress_result.bin`
Expected: prints `ALLOC (from tools/stress_result.bin): active=… erase min/max/avg=…`,
opens the wear chart, and writes `docs/wear-leveling.png`.

- [ ] **Step 4: Commit**

```bash
git add tools/flash_analyzer.py
git commit -m "feat(analyzer): --file mode to chart a saved allocation-table blob"
```

---

### Task 4: Verification

**Files:** none

- [ ] **Step 1: Full check**

```bash
make test                                   # ALL TESTS PASSED (stub extraction intact)
python3 tests/test_analyzer.py | tail -1    # ALL TESTS PASSED
python3 tests/test_stress.py                # test_stress: PASS (mismatches 0)
make stress                                 # SUMMARY mismatches: 0, saves stress_result.bin
python3 -m py_compile tools/flash_analyzer.py && echo OK
python3 tools/flash_analyzer.py --file tools/stress_result.bin   # wear chart + stats
```

- [ ] **Step 2: Finish the branch**

Use `superpowers:finishing-a-development-branch`.

---

## Self-review notes

- **Spec coverage:** flash_stub extraction + `g_phys_erase` → Task 1; stress binary (cyclic integrity + reformat-on-full + fill-to-capacity + summary + stress_result.bin) → Task 2; `make stress` → Task 2; `--file` mode → Task 3; `test_stress.py` (50-cycle CI) → Task 2; verification → Task 4.
- **Type consistency:** `g_phys_erase[FLASH_TOTAL_SECTORS]` (2048) indexed by physical sector; blob packs `state` (byte) + LE `erase_count` matching `parse_alloc_table`'s `<BI`; data sectors are physical index 4..2047 in both the stats loop and the blob; `--file` reuses `parse_alloc_table`/`wear_chart`/`SECTOR_ACTIVE` defined earlier in `flash_analyzer.py`.
- **Constraint check:** only the six allowed files; `fs.cpp`/`fs.h`/`flash.cpp` untouched; `stress_result.bin` matches `*.bin` gitignore; `make test` target unchanged (same-dir header include needs no `-I`).

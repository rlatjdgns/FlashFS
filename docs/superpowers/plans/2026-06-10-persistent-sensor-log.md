# Persistent Sensor Log Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the flash file system a reboot-persistent sensor log — one file per slot (`file_id = seq % MAX_FILES`) — so the readback never emits garbage and prior readings survive a power cycle.

**Architecture:** Create 32 slot files at boot (idempotent). Each reading overwrites its slot's file via the existing `fs_write`. A one-line `fs_create` correctness fix makes `fs_read` return -1 for created-but-unwritten slots, so the readback can safely skip them. A host-side RAM-backed-flash test reproduces the root cause and guards the fix; firmware integration is verified on hardware via `tools/listen.py`.

**Tech Stack:** C++ bare-metal (STM32F103, `arm-none-eabi-g++`); host unit test compiled with `c++` (clang) against a RAM flash stub; `pyserial` listener for hardware verification.

**Design spec:** `docs/superpowers/specs/2026-06-10-persistent-sensor-log-design.md`

---

## Pre-flight

We are on `main`. Per project policy, create a branch before the first commit:

```bash
git checkout -b fix-persistent-sensor-log
```

The test binary is written to `build/` — do not `git add` it; the commit steps add only source files.

## File structure

- Create: `tests/test_fs.cpp` — host unit tests + RAM-backed flash stub (one responsibility: exercise `fs.cpp` logic off-target).
- Modify: `Makefile` — add a host `test` target (separate from the ARM firmware build).
- Modify: `src/fs.cpp` — one-line correctness fix in `fs_create` (init `firstpage_addr`).
- Modify: `src/main.cpp` — create 32 slot files; rewrite the readback loop to scan all slots, initialize the buffer, and honor `fs_read`'s return value.

---

### Task 1: Host test harness + failing root-cause test

**Files:**
- Create: `tests/test_fs.cpp`
- Modify: `Makefile` (add `test` target)

- [ ] **Step 1: Write the test harness with the first (failing) test**

Create `tests/test_fs.cpp`:

```cpp
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
```

- [ ] **Step 2: Add the `test` target to `Makefile`**

Append to `Makefile`:

```makefile

# --- Host unit tests (off-target; uses RAM-backed flash stub) ---
HOSTCXX ?= c++
test:
	mkdir -p build
	$(HOSTCXX) -std=c++17 -I src -O0 -g -Wall -o build/test_fs tests/test_fs.cpp src/fs.cpp
	./build/test_fs
```

- [ ] **Step 3: Run the test to verify it FAILS**

Run: `make test`
Expected: builds cleanly, then **FAILS** — output contains
`FAIL tests/test_fs.cpp:NN  rc == -1` and `1 CHECK(s) FAILED`, exit code 1.
(Reason: `fs_create` zero-inits `firstpage_addr = 0`, so `fs_read` scans sector 0
and returns 0 instead of -1 — this is the root cause reproduced.)

- [ ] **Step 4: Commit**

```bash
git add tests/test_fs.cpp Makefile
git commit -m "test: failing host test reproducing unwritten-slot read bug"
```

---

### Task 2: Fix `fs_create` so unwritten slots report failure

**Files:**
- Modify: `src/fs.cpp` (in `fs_create`, the "Build new entry in RAM" block)

- [ ] **Step 1: Add the `firstpage_addr` sentinel init**

In `src/fs.cpp`, inside `fs_create`, find:

```cpp
            //Build new entry in RAM
            for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++)
                ((uint8_t*)&entry)[k] = 0;
            entry.status = FILE_ACTIVE;
            entry.file_id = i;
```

Change it to add one line:

```cpp
            //Build new entry in RAM
            for(uint32_t k = 0; k < sizeof(DirectoryEntry); k++)
                ((uint8_t*)&entry)[k] = 0;
            entry.status = FILE_ACTIVE;
            entry.file_id = i;
            entry.firstpage_addr = 0xFFFFFFFF;   // no data yet: fs_read returns -1 until first write (0 is a real addr: the superblock)
```

- [ ] **Step 2: Run the test to verify it PASSES**

Run: `make test`
Expected: `ALL TESTS PASSED`, exit code 0.
(`fs_read` now hits the `start_addr == 0xFFFFFFFF` guard at the top of `fs_read` and returns -1.)

- [ ] **Step 3: Commit**

```bash
git add src/fs.cpp
git commit -m "fix(fs): mark unwritten files with firstpage_addr sentinel so fs_read returns -1"
```

---

### Task 3: Characterization tests — roundtrip, persistence, id assignment

**Files:**
- Modify: `tests/test_fs.cpp` (add three tests + call them)

- [ ] **Step 1: Add three tests above `main()`**

In `tests/test_fs.cpp`, add these functions immediately before `int main()`:

```cpp
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
```

- [ ] **Step 2: Call them from `main()`**

Change `main()` in `tests/test_fs.cpp` to:

```cpp
int main() {
    test_unwritten_file_returns_error();
    test_roundtrip_nonzero_fileid();
    test_persistence_across_reinit();
    test_create_assigns_sequential_ids();
    if (g_failures == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    printf("%d CHECK(s) FAILED\n", g_failures);
    return 1;
}
```

- [ ] **Step 3: Run the tests to verify they PASS**

Run: `make test`
Expected: `ALL TESTS PASSED`, exit code 0.

- [ ] **Step 4: Commit**

```bash
git add tests/test_fs.cpp
git commit -m "test: roundtrip, reboot-persistence, and sequential-id characterization"
```

---

### Task 4: Wire up `main.cpp` — 32 slot files + safe readback

**Files:**
- Modify: `src/main.cpp` (replace the single `fs_create`; rewrite the readback loop)

- [ ] **Step 1: Replace the single file create with 32 slot files**

In `src/main.cpp`, find:

```cpp
    uart_send_string("fs create\n");   
    fs_create("sensor.bin");  
```

Replace with:

```cpp
    uart_send_string("fs create\n");
    // One file per slot: file_id 0..MAX_FILES-1. Idempotent across reboots.
    char name[4];
    name[0] = 's';
    for(uint8_t i = 0; i < MAX_FILES; i++){
        name[1] = '0' + (i / 10);
        name[2] = '0' + (i % 10);
        name[3] = '\0';
        fs_create(name);
    }
```

- [ ] **Step 2: Rewrite the readback loop to scan all slots safely**

In `src/main.cpp`, find:

```cpp
        if(seq > 0 && seq % 5 == 0){
            for(int j = seq - 4; j <= seq; j++){
                SensorReading r;
                fs_read(j % MAX_FILES, (uint8_t*)&r, sizeof(SensorReading));
                uart_send_byte((r.temp >> 24) & 0xFF);
                uart_send_byte((r.temp >> 16) & 0xFF);
                uart_send_byte((r.temp >> 8) & 0xFF);
                uart_send_byte((r.temp >> 0) & 0xFF);
            }
        }
```

Replace with:

```cpp
        if(seq > 0 && seq % 5 == 0){
            for(uint8_t fid = 0; fid < MAX_FILES; fid++){
                SensorReading r;
                r.seq = 0;
                r.temp = 0;
                // fs_read returns 0 only when real data was copied; -1 for an
                // unwritten/missing slot or a CRC failure -> skip it.
                if(fs_read(fid, (uint8_t*)&r, sizeof(SensorReading)) == 0){
                    uart_send_byte((r.temp >> 24) & 0xFF);
                    uart_send_byte((r.temp >> 16) & 0xFF);
                    uart_send_byte((r.temp >> 8) & 0xFF);
                    uart_send_byte((r.temp >> 0) & 0xFF);
                }
            }
        }
```

- [ ] **Step 3: Build the firmware to verify it compiles**

Run: `make all`
Expected: builds `flashfs.elf` with no errors. (Note: `make all` requires
`arm-none-eabi-g++`. If `make test` was run before, run `make clean` first only if
object files conflict — the host and ARM builds use different object paths, so
normally no clean is needed.)

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: per-slot persistent sensor log; readback skips empty/failed slots"
```

---

### Task 5: Hardware verification (success criteria)

**Files:** none (verification only)

This is the spec's acceptance gate. Requires the board on `/dev/cu.usbserial-0001`.
Close any other listener first (the port is single-access).

- [ ] **Step 1: Flash the firmware**

Run: `make flash`
Expected: openocd programs and verifies `flashfs.elf`, board resets.

- [ ] **Step 2: Capture a full fill (>32 readings) and check for implausible values**

Run (≈ capture spanning at least 32 readings; live cadence is ~24 s, so ~15 min for a full fill — adjust the sleep as needed):

```bash
python3 tools/listen.py /dev/cu.usbserial-0001 > /tmp/verify.txt 2>&1 &
LPID=$!; sleep 900; kill $LPID 2>/dev/null; wait $LPID 2>/dev/null
echo "implausible lines:"; grep -c "implausible" /tmp/verify.txt
echo "0x00000000 lines:";  grep -c "read returned 0" /tmp/verify.txt
echo "I2C timeout tags:";  grep -c "I2C:to" /tmp/verify.txt
```

Expected: **0** implausible lines, **0** "read returned 0" lines, **0** I2C tags.
Success criterion 1 met.

- [ ] **Step 3: Verify persistence across a power cycle**

Power-cycle the board (unplug/replug, or `make flash` to reset) while a listener
runs, and confirm that after boot the readback bursts print previously-seen
temperatures (the prior session's stored readings) before new writes overwrite
those slots. Success criterion 2 met.

- [ ] **Step 4: Finish the branch**

Use the `superpowers:finishing-a-development-branch` skill to merge/PR per your
preference.

---

## Self-review notes

- **Spec coverage:** create-32-files (Task 4 Step 1), keep `fs_write(seq % MAX_FILES)` (unchanged, exercised in Task 3 roundtrip), readback-all-slots + init `r` + check return (Task 4 Step 2), `fs_create` `firstpage_addr` fix (Task 2). Verification criteria → Task 5. Limitations are accepted/out-of-scope, no tasks needed.
- **Types:** `SensorReading {uint32_t seq; int32_t temp;}` (demo.h), `fs_read`/`fs_write` take `(uint8_t file_id, uint8_t* buf, uint32_t len)` and return `int` — used consistently across tasks.
- **Flash stub:** defines exactly the three functions `fs.cpp` calls; `flash_write_enable`/`flash_wait_busy` are internal to `flash.cpp` and not referenced by `fs.cpp`, so they are not stubbed.

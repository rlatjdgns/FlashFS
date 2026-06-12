# Flash FS Host-Side Stress Test — Design

Date: 2026-06-12
Status: Approved (design)

## Problem

The FS has unit tests but no sustained-load test. We want a host-side stress test
(using the existing RAM flash stub) that hammers `fs_write`/`fs_read`, verifies
integrity over many cycles, characterizes write capacity, and produces a wear-
leveling chart — without hardware.

## Key constraint (drives the design)

The FS has **no sector reclamation**: each `fs_write` consumes a fresh free sector
and never frees the old one. So:
- Write capacity is ~2044 data sectors; after that `fs_write` returns -1.
- 32 writes/cycle × 500 cycles = 16,000 writes >> 2044 → the test **must reformat
  on full** (`flash_reset` + `fs_init` + recreate 32 files) to keep going.
- The FS's own `AllocationEntry.erase_count` resets on every reformat, so it can't
  show cumulative wear. We therefore track **physical** erases in the stub.

## Constraints / rules

- Use the existing RAM flash stub; don't reinvent it.
- No FS logic changes (`fs.cpp`/`fs.h`/`flash.cpp` untouched).
- C++ for the stress binary; Python for the analyzer extension. No new abstractions.

## Components

### 1. `tests/flash_stub.h` (new) — extract + instrument

Move the RAM stub currently inline in `tests/test_fs.cpp` into this header (both
the FS unit test and the stress binary include it). Definitions (non-static; each
binary includes it in exactly one TU):

- `uint8_t flash_mem[9*1024*1024]`
- `uint32_t g_phys_erase[2048]` — cumulative physical erase count per sector,
  **NOT** cleared by `flash_reset`.
- `flash_reset()` — `memset(flash_mem, 0xFF, …)` only (leaves `g_phys_erase`).
- `flash_sector_erase(addr)` — erase the 4 KB sector AND `g_phys_erase[addr/4096]++`
  (guard index < 2048).
- `flash_page_program`, `flash_read_data` — unchanged from the current stub.

`tests/test_fs.cpp` swaps its inline stub for `#include "flash_stub.h"`. Same-dir
include, so no Makefile `-I` change.

### 2. `tests/stress_test.cpp` (new) — `./stress_test [N=500]`

Includes `fs.h`, `flash.h`, `flash_stub.h`. Uses `WLEN = 16`-byte known payloads:
`data[i] = (uint8_t)(seq * 31 + i)`.

`reformat()` = `flash_reset(); fs_init();` then create 32 files `s00`..`s31`.

- **Phase 1 — cyclic integrity:** `reformat()`, then N cycles × 32 slots. For each
  write: build payload from a global `seq`; `if (fs_write(slot, data, WLEN) != 0) {
  reformat(); fs_write(slot, data, WLEN); }`; `total_writes++`; immediately
  `fs_read(slot, rb, WLEN)` and `memcmp` — mismatch → `mismatches++`; `total_reads++`.
  Print progress every 50 cycles.
- **Phase 2 — fill-to-capacity:** `reformat()`; write `slot = k % 32` until
  `fs_write` returns -1; record `max_capacity_writes = k`. Then read all 32 slots and
  assert each returns 0 (no corruption from hitting the limit).
- **Summary** (stdout, parseable):
  ```
  total_writes: <N*32>
  total_reads:  <N*32>
  mismatches:   0
  max_capacity_writes: <~2044>
  erase_count(data sectors) min=<> max=<> avg=<>
  saved tools/stress_result.bin
  ```
  `erase_count` stats are over `g_phys_erase[4..2047]` (data sectors).
- **Exit 1** if `mismatches > 0`, else 0.

### 3. `tools/stress_result.bin`

Built by the stress test as a 12 KB allocation-table-layout blob (2048
`AllocationEntry` × 5 bytes, padded). For alloc index `j`: `state = SECTOR_ACTIVE`
if `g_phys_erase[j] > 0` else `SECTOR_FREE`; `erase_count = g_phys_erase[j]`. Packed
byte-wise (state at `j*5`, little-endian `erase_count` at `j*5+1`). This carries the
**cumulative physical wear** in the format `parse_alloc_table` already reads.
(Already covered by `*.bin` in `.gitignore`.)

### 4. `tools/flash_analyzer.py` — `--file` mode

`python3 tools/flash_analyzer.py --file tools/stress_result.bin`: read the file,
`parse_alloc_table`, print allocation stats (active count, erase min/max/avg), and
`wear_chart(...)` → `docs/wear-leveling.png`. No serial. Live `<port> <baud>` mode
unchanged. `main()` dispatches on `argv[1] == "--file"`.

### 5. `tests/test_stress.py` (new, CI)

Standalone: compile `stress_test.cpp` + `fs.cpp` (host `c++`), run the binary with
`50` (cwd = repo root so it writes `tools/stress_result.bin`), assert exit code 0
and `mismatches: 0` in stdout. Run: `python3 tests/test_stress.py`.

### 6. `Makefile` — `stress` target

```makefile
stress:
	mkdir -p build
	$(HOSTCXX) -std=c++17 -I src -O0 -g -o build/stress_test tests/stress_test.cpp src/fs.cpp
	./build/stress_test
```
(`flash_stub.h` resolves via `stress_test.cpp`'s dir; `HOSTCXX` already defined for
`test`.) The `test` target is unchanged.

## Data flow

```
stress_test: reformat -> 500×32 writes (reformat on full) -> verify each read
   -> g_phys_erase[] (cumulative)  -> summary + tools/stress_result.bin (alloc layout)
flash_analyzer --file tools/stress_result.bin -> parse_alloc_table -> wear chart
```

## Error handling

- `fs_write == -1` (full) → reformat + retry; not a mismatch.
- Mismatch (read != written) → counted; nonzero exit.
- Phase-2 read failures after fill → assertion failure (corruption signal).

## Files touched

`tests/flash_stub.h` (new), `tests/test_fs.cpp` (swap stub for include),
`tests/stress_test.cpp` (new), `tests/test_stress.py` (new),
`tools/flash_analyzer.py` (--file), `Makefile` (stress target).
Not `fs.cpp`/`fs.h`/`flash.cpp`.

## Expected result (honest)

Lowest-free-index allocation + full passes each fill → **fairly uniform** wear
(~7–9 erases/sector, small step at the last partial fill), not a dramatic spike.
`mismatches: 0`, `max_capacity_writes ≈ 2044`.

## Verification

1. `python3 tests/test_stress.py` → PASS (exit 0, mismatches 0).
2. `make test` still green (stub extraction didn't break it).
3. `make stress` → summary with `mismatches: 0`, writes `tools/stress_result.bin`.
4. `python3 tools/flash_analyzer.py --file tools/stress_result.bin` → wear chart.

# Flash Image Analyzer — Design

Date: 2026-06-10
Status: Approved (design)

## Problem

There is no way to inspect the on-flash file system image off-target. Debugging
wear leveling, directory state, and page CRCs requires reading the raw flash and
decoding its structures on a host.

## Goal

A UART-triggered firmware dump that streams the raw flash image as-is, plus a
procedural Python analyzer that decodes and visualizes it (superblock, directory,
allocation table + wear stats, wear-leveling chart, per-page CRC checks).

## Constraints

- No FS logic changes (`fs.cpp`/`fs.h` untouched).
- Firmware addition minimal — only the dump path, no refactor.
- `flash.cpp` untouched (reuse `flash_read_data`).
- Simple procedural Python, no classes.

## Struct layouts (little-endian; verified against `fs.h`)

Only `AllocationEntry` is `__attribute__((packed))`; the rest use natural alignment.

| Struct | `struct` format | Size |
|---|---|---|
| `Superblock` | `<IBxHHHB3xI` | 20 |
| `DirectoryEntry` | `<16sB3xIIIB3x` | 36 |
| `AllocationEntry` | `<BI` | 5 |
| `PageHeader` | `<BB2xIBxH` | 12 |

Field order — Superblock: magic, version, total_sectors, sector_size, page_size,
num_files, alloc_table_addr. Directory: filename[16], file_id, file_size,
firstpage_addr, created, status. Alloc: state, erase_count. Page: state, file_id,
file_offset, data_length, crc.

Constants (from `fs.h`): `FLASH_TOTAL_SECTORS=2048`, `FLASH_SECTOR_SIZE=4096`,
`FLASH_PAGE_SIZE=256`, `FLASH_DATA_SIZE=244`, `MAX_FILES=32`,
`SUPERBLOCK_ADDR=0x0`, `ALLOC_TABLE_ADDR=0x1000`, `DATA_START_ADDR=0x4000`.
States: `FILE_ACTIVE=0x01`; `SECTOR_FREE=0x00`, `SECTOR_ACTIVE=0x01`,
`SECTOR_FULL=0x02`; `PAGE_VALID=0xFE`, `PAGE_ERASED=0xFF`.

Sector ↔ alloc-index mapping (from `fs.cpp`): data sector `i` (address
`DATA_START_ADDR + i*4096`) has its `AllocationEntry` at alloc index `i+4`
(`ALLOC_TABLE_ADDR + (i+4)*5`), for `i = 0 .. FLASH_TOTAL_SECTORS-4-1` (0..2043).

## Part 1 — Firmware

### `uart.cpp` / `uart.h`

- Enable the receiver: set `RE` (CR1 bit 2) in `uart_init` (currently UE+TE only).
  PA10 is already configured as floating-input RX.
- `int uart_rx_ready()` — return `USART1_SR & (1<<5)` (RXNE): byte waiting?
- `uint8_t uart_receive()` — spin until RXNE, return `DR`.

### `main.cpp`

After `uart_init()` + `spi_init()` and **before `fs_init()`**, poll for the dump
trigger in a bounded ~2 s busy-loop:

```
for a bounded iteration count (~2 s):
    if uart_rx_ready() and uart_receive() == 0xDD:
        flash_dump();
        while(1);            // halt — do not touch fs_init or the sensor loop
```

Running before `fs_init` is the safety property: `fs_init` reformats on missing/
stale magic, which would destroy the image. Dump mode never writes.

`flash_dump()` (in `main.cpp`, reuses `flash_read_data`, `uart_send_byte`,
`uart_send_u32`):

1. `uart_send_string("DUMP:\n")`
2. dump sector 0 (4096 B) then sectors 1–3 (12288 B), read in 256 B chunks into a
   local `uint8_t buf[256]`, sending each chunk (no 4 KB RAM buffer)
3. for `i = 0..2043`: read the 5-byte `AllocationEntry` at
   `ALLOC_TABLE_ADDR + (i+4)*5`; if `state == SECTOR_ACTIVE`,
   `uart_send_u32(DATA_START_ADDR + i*4096)` (big-endian) then dump that sector
   (4096 B, 256 B chunks)
4. `uart_send_string("DUMP_END:\n")`

## Wire format

```
"DUMP:\n"
[4096 B]                 sector 0  (superblock + directory)
[12288 B]               sectors 1–3  (allocation table)
N × ([4 B BE addr][4096 B])   active data sectors
"DUMP_END:\n"
```

N is deterministic: the analyzer parses the received allocation table and counts
`SECTOR_ACTIVE` among data sectors, then reads exactly N records.

## Part 2 — Analyzer `tools/flash_analyzer.py`

Procedural, no classes. `python3 tools/flash_analyzer.py <port> 115200`.

**Pure functions (module level, only `import struct`)** — imported by the test:
- `crc16(data)` — replicate `fs.cpp`: `crc=0xFFFF`; per byte `crc^=byte<<8`;
  8× `crc = ((crc<<1)^0x1021) if crc&0x8000 else crc<<1`, masked to 16 bits.
- `parse_superblock(b)` → dict(magic, version, total_sectors, sector_size,
  page_size, num_files, alloc_table_addr) from `b[0:20]`.
- `parse_directory(sector0)` → list of dicts for the 32 entries at
  `sector0[20:20+32*36]` with `status == FILE_ACTIVE` (filename trimmed at NUL).
- `parse_alloc_table(b)` → list of `(index, state, erase_count)` for 2048 entries.
- `parse_sector_pages(sector_bytes)` → list of page dicts; for each 256 B page with
  `state == PAGE_VALID`: file_id, file_offset, data_length, and
  `crc_ok = crc16(payload[:data_length]) == header_crc`; stop at `PAGE_ERASED`.

**I/O + presentation (`main()`, matplotlib imported lazily here):**
- `read_dump(ser)` — send `0xDD` repeatedly until `DUMP:\n` is seen (overall
  timeout ~10 s; prompt the user to press RESET), then read 4096 + 12288 +
  N×(4+4096) bytes + `DUMP_END:\n`. Returns sector0, alloc_bytes, and the list of
  `(addr, 4096-byte data)` active sectors.
- Print: **Superblock** (magic/version/geometry); **Directory** table (filename,
  file_id, file_size, firstpage_addr); **Allocation table** stats (min/max/avg
  erase_count, active count); **Data pages** per active sector (file_id,
  file_offset, data_length, CRC PASS/FAIL).
- **Wear-leveling chart** (matplotlib bar): x = sector index, y = erase_count,
  color by state — green=free, blue=active, orange=full. Save to
  `docs/wear-leveling.png` and `plt.show()`.

## Part 3 — Test + docs

- `tests/test_analyzer.py` — standalone asserts, no hardware/matplotlib/serial.
  Build a synthetic dump with `struct.pack`: a valid superblock; 2 active directory
  entries; an allocation table with a few `SECTOR_ACTIVE` entries with distinct
  erase counts (plus frees); active-sector bytes containing one `PAGE_VALID` page
  with a correct CRC and one with a deliberately wrong CRC. Call the pure parse
  functions and assert: superblock fields, directory list, alloc min/max/avg, and
  page CRC pass/fail (one PASS, one FAIL). Run: `python3 tests/test_analyzer.py`.
- `README.md` — add `flash_analyzer.py` to the tools docs: dump-and-inspect the
  on-flash image (superblock/directory/alloc table + wear-leveling chart + page
  CRCs), with the reset-then-run trigger note.

## Trigger workflow

The board only listens for `0xDD` in the first ~2 s after reset. The analyzer spams
`0xDD` on connect while you press the board's RESET button, and stops once it sees
`DUMP:\n`.

## Files touched

`uart.cpp`, `uart.h`, `main.cpp`, `tools/flash_analyzer.py` (new),
`tests/test_analyzer.py` (new), `README.md`. Not `flash.cpp`, `fs.cpp`, `fs.h`.

## Verification

1. `python3 tests/test_analyzer.py` → all asserts pass (no hardware).
2. `make all` builds.
3. Hardware: reset board + run analyzer → prints superblock/directory/alloc/pages,
   shows the wear chart, page CRCs report PASS for valid pages.

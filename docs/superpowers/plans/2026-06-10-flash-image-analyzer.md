# Flash Image Analyzer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A UART-triggered firmware dump of the raw flash image and a procedural Python analyzer that decodes the superblock, directory, allocation table (with wear stats + chart), and per-page CRCs.

**Architecture:** On boot, before `fs_init`, the firmware polls UART RX for `0xDD` (~2 s); if seen it streams the raw flash over UART and halts. `tools/flash_analyzer.py` triggers and parses that dump via pure `struct`-based functions (unit-tested) plus matplotlib for the wear chart. No FS logic or `flash.cpp` changes.

**Tech Stack:** C++ bare-metal (`arm-none-eabi-g++`), Python 3 + pyserial + matplotlib.

**Design spec:** `docs/superpowers/specs/2026-06-10-flash-image-analyzer-design.md`

**Constraint:** Touch ONLY `src/uart.h`, `src/uart.cpp`, `src/main.cpp`, `tools/flash_analyzer.py`, `tests/test_analyzer.py`, `README.md`. Do NOT change `fs.cpp`, `fs.h`, or `flash.cpp`.

---

## Pre-flight

```bash
git checkout -b feat-flash-analyzer
```

---

### Task 1: Analyzer pure parsers + failing test

**Files:**
- Create: `tools/flash_analyzer.py` (parsers only, this task)
- Create: `tests/test_analyzer.py`

- [ ] **Step 1: Write the failing test**

Create `tests/test_analyzer.py`:

```python
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import flash_analyzer as fa

failures = 0
def check(cond, msg):
    global failures
    if not cond:
        print(f"FAIL: {msg}")
        failures += 1

def build_sector0():
    sb = fa.SB.pack(0xDEADBEEF, 2, 2048, 4096, 256, 2, 0x1000)
    d = b""
    d += fa.DE.pack(b"sensor.bin", 0, 8, 0x4000, 0, fa.FILE_ACTIVE)
    d += fa.DE.pack(b"log.bin", 1, 16, 0x5000, 0, fa.FILE_ACTIVE)
    for i in range(2, fa.MAX_FILES):
        d += fa.DE.pack(b"", i, 0, 0, 0, 0xFF)            # inactive
    s = sb + d
    return s + b"\xFF" * (fa.SECTOR_SIZE - len(s))

def build_alloc():
    a = bytearray(b"\x00" * (fa.TOTAL_SECTORS * fa.AE.size))   # all free, erase 0
    a[4 * fa.AE.size:5 * fa.AE.size] = fa.AE.pack(fa.SECTOR_ACTIVE, 3)   # data sector 0
    a[5 * fa.AE.size:6 * fa.AE.size] = fa.AE.pack(fa.SECTOR_ACTIVE, 7)   # data sector 1
    return bytes(a) + b"\xFF" * (3 * fa.SECTOR_SIZE - len(a))

def make_page(state, fid, foff, payload, crc):
    page = fa.PH.pack(state, fid, foff, len(payload), crc) + payload
    return page + b"\xFF" * (fa.PAGE_SIZE - len(page))

def make_sector(first_page):
    return first_page + b"\xFF" * (fa.SECTOR_SIZE - len(first_page))

def test_parsers():
    sector0 = build_sector0()
    sb = fa.parse_superblock(sector0)
    check(sb["magic"] == 0xDEADBEEF, f"magic: {sb['magic']:#x}")
    check(sb["version"] == 2, "version")
    check(sb["total_sectors"] == 2048 and sb["sector_size"] == 4096 and sb["page_size"] == 256, "geometry")

    d = fa.parse_directory(sector0)
    check(len(d) == 2, f"dir count: {len(d)}")
    check(d[0]["filename"] == "sensor.bin" and d[0]["file_id"] == 0 and d[0]["file_size"] == 8 and d[0]["firstpage_addr"] == 0x4000, f"dir0: {d[0]}")
    check(d[1]["filename"] == "log.bin" and d[1]["file_id"] == 1, f"dir1: {d[1]}")

    a = fa.parse_alloc_table(build_alloc())
    actives = [e for e in a if e[1] == fa.SECTOR_ACTIVE]
    check(len(actives) == 2, f"active count: {len(actives)}")
    erases = [e[2] for e in actives]
    check(min(erases) == 3 and max(erases) == 7, f"erase span: {erases}")

    payload0 = bytes([0, 0, 0, 0, 0xF5, 0x0A, 0, 0])
    p0 = fa.parse_sector_pages(make_sector(make_page(fa.PAGE_VALID, 0, 0, payload0, fa.crc16(payload0))))
    check(len(p0) == 1 and p0[0]["crc_ok"] is True and p0[0]["data_length"] == 8 and p0[0]["file_id"] == 0, f"good page: {p0}")

    payload1 = bytes([1, 2, 3, 4, 5, 6, 7, 8])
    bad = (fa.crc16(payload1) ^ 0xFFFF) & 0xFFFF
    p1 = fa.parse_sector_pages(make_sector(make_page(fa.PAGE_VALID, 1, 0, payload1, bad)))
    check(len(p1) == 1 and p1[0]["crc_ok"] is False, f"bad page: {p1}")

test_parsers()
if failures == 0:
    print("ALL TESTS PASSED")
    sys.exit(0)
print(f"{failures} CHECK(s) FAILED")
sys.exit(1)
```

- [ ] **Step 2: Run the test to verify it FAILS**

Run: `python3 tests/test_analyzer.py`
Expected: `ModuleNotFoundError: No module named 'flash_analyzer'`.

- [ ] **Step 3: Write the parsers**

Create `tools/flash_analyzer.py`:

```python
#!/usr/bin/env python3
"""Dump-and-inspect the STM32 flash-FS image.

Usage: python3 tools/flash_analyzer.py <port> 115200
Press the board's RESET button when prompted; the analyzer sends 0xDD to trigger
dump mode, then decodes the raw flash image.
"""
import struct

# --- geometry / states (mirror fs.h) ---
SECTOR_SIZE, PAGE_SIZE, PAGE_HDR = 4096, 256, 12
TOTAL_SECTORS, MAX_FILES = 2048, 32
SUPERBLOCK_SIZE = 20
DATA_START = 0x4000
FILE_ACTIVE = 0x01
SECTOR_FREE, SECTOR_ACTIVE, SECTOR_FULL = 0x00, 0x01, 0x02
PAGE_VALID, PAGE_ERASED = 0xFE, 0xFF

SB = struct.Struct("<IBxHHHB3xI")     # 20: magic, version, total, ssize, psize, nfiles, alloc_addr
DE = struct.Struct("<16sB3xIIIB3x")   # 36: filename, file_id, file_size, firstpage_addr, created, status
AE = struct.Struct("<BI")             # 5 : state, erase_count
PH = struct.Struct("<BB2xIBxH")       # 12: state, file_id, file_offset, data_length, crc


def crc16(data):
    """CRC16-CCITT, init 0xFFFF, poly 0x1021 — identical to fs.cpp crc16()."""
    crc = 0xFFFF
    for b in data:
        crc ^= (b << 8)
        crc &= 0xFFFF
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def parse_superblock(sector0):
    magic, version, total, ssize, psize, nfiles, alloc_addr = SB.unpack_from(sector0, 0)
    return {"magic": magic, "version": version, "total_sectors": total,
            "sector_size": ssize, "page_size": psize, "num_files": nfiles,
            "alloc_table_addr": alloc_addr}


def parse_directory(sector0):
    out = []
    for i in range(MAX_FILES):
        name, fid, fsize, firstpage, created, status = DE.unpack_from(sector0, SUPERBLOCK_SIZE + i * DE.size)
        if status == FILE_ACTIVE:
            out.append({"filename": name.split(b"\x00", 1)[0].decode("ascii", "replace"),
                        "file_id": fid, "file_size": fsize, "firstpage_addr": firstpage})
    return out


def parse_alloc_table(alloc_bytes):
    return [(i, *AE.unpack_from(alloc_bytes, i * AE.size)) for i in range(TOTAL_SECTORS)]


def parse_sector_pages(sector_bytes):
    out = []
    for p in range(SECTOR_SIZE // PAGE_SIZE):
        off = p * PAGE_SIZE
        state, fid, foff, dlen, crc = PH.unpack_from(sector_bytes, off)
        if state == PAGE_ERASED:
            break
        if state == PAGE_VALID:
            payload = sector_bytes[off + PAGE_HDR: off + PAGE_HDR + dlen]
            out.append({"file_id": fid, "file_offset": foff, "data_length": dlen,
                        "crc": crc, "crc_ok": crc16(payload) == crc})
    return out
```

- [ ] **Step 4: Run the test to verify it PASSES**

Run: `python3 tests/test_analyzer.py`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
git add tools/flash_analyzer.py tests/test_analyzer.py
git commit -m "feat: flash-image parsers with unit tests"
```

---

### Task 2: Analyzer dump reader, report, and wear chart

**Files:**
- Modify: `tools/flash_analyzer.py` (append I/O + presentation)

- [ ] **Step 1: Append `read_dump`, reporting, chart, and `main`**

Append to `tools/flash_analyzer.py`:

```python
def read_dump(ser):
    """Trigger dump mode (spam 0xDD until 'DUMP:\\n'), then read the framed image."""
    import time
    buf = bytearray()
    deadline = time.time() + 12
    while b"DUMP:\n" not in buf:
        ser.write(b"\xDD")
        buf += ser.read(64)
        if time.time() > deadline:
            raise SystemExit("No DUMP: seen — press the board RESET button and rerun.")
    buf = buf[buf.index(b"DUMP:\n") + len(b"DUMP:\n"):]

    def take(n):
        nonlocal buf
        while len(buf) < n:
            buf += ser.read(n - len(buf))
        out, buf = bytes(buf[:n]), buf[n:]
        return out

    sector0 = take(SECTOR_SIZE)
    alloc = take(3 * SECTOR_SIZE)
    n_active = sum(1 for (_, s, _) in parse_alloc_table(alloc) if s == SECTOR_ACTIVE)
    sectors = []
    for _ in range(n_active):
        addr = struct.unpack(">I", take(4))[0]
        sectors.append((addr, take(SECTOR_SIZE)))
    return sector0, alloc, sectors


def wear_chart(alloc_entries, path):
    import matplotlib
    matplotlib.use("Agg") if False else None
    import matplotlib.pyplot as plt
    color = {SECTOR_FREE: "green", SECTOR_ACTIVE: "blue", SECTOR_FULL: "orange"}
    idx = [i for (i, _, _) in alloc_entries]
    erase = [e for (_, _, e) in alloc_entries]
    cols = [color.get(s, "gray") for (_, s, _) in alloc_entries]
    fig, ax = plt.subplots()
    ax.bar(idx, erase, color=cols, width=1.0)
    ax.set_title("Flash Wear Leveling — erase count per sector")
    ax.set_xlabel("Sector index")
    ax.set_ylabel("Erase count")
    fig.savefig(path, dpi=120, bbox_inches="tight")
    plt.show()


def main():
    import sys, serial
    if len(sys.argv) != 3:
        sys.exit("usage: python3 flash_analyzer.py <port> <baud>")
    ser = serial.Serial(sys.argv[1], int(sys.argv[2]), timeout=0.3)
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


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify it compiles and the test still passes**

Run: `python3 -m py_compile tools/flash_analyzer.py && python3 tests/test_analyzer.py | tail -1`
Expected: `ALL TESTS PASSED` (importing the module still pulls no serial/matplotlib).

- [ ] **Step 3: Commit**

```bash
git add tools/flash_analyzer.py
git commit -m "feat: dump reader, report, and wear-leveling chart"
```

---

### Task 3: Firmware UART receive

**Files:**
- Modify: `src/uart.h`
- Modify: `src/uart.cpp`

- [ ] **Step 1: Declare the RX functions in `uart.h`**

Add to `src/uart.h` (after the existing declarations):

```c
int uart_rx_ready();
uint8_t uart_receive();
```

- [ ] **Step 2: Enable RE and implement RX in `uart.cpp`**

In `src/uart.cpp`, change the CR1 enable line in `uart_init` from:

```cpp
    //Enable UART and transmitter for USART1 CR 1 
    *(volatile uint32_t*)0x4001380C |= (1<<13)|(1<<3);
```

to (add RE, bit 2):

```cpp
    //Enable UART, transmitter, and receiver for USART1 CR1
    *(volatile uint32_t*)0x4001380C |= (1<<13)|(1<<3)|(1<<2);
```

Then append to `src/uart.cpp`:

```cpp
//Byte waiting in the receive register? (SR bit5 = RXNE)
int uart_rx_ready(){
    return (*(volatile uint32_t*)0x40013800) & (1<<5);
}

//Block until a byte arrives, then return it from DR.
uint8_t uart_receive(){
    while(!((*(volatile uint32_t*)0x40013800) & (1<<5)));
    return *(volatile uint32_t*)0x40013804;
}
```

- [ ] **Step 3: Build**

Run: `make all`
Expected: links `flashfs.elf` with no errors.

- [ ] **Step 4: Commit**

```bash
git add src/uart.h src/uart.cpp
git commit -m "feat(uart): enable receiver with uart_rx_ready/uart_receive"
```

---

### Task 4: Firmware dump trigger + flash_dump

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the flash_read_data forward declaration and dump routine**

In `src/main.cpp`, find:

```cpp
void spi_init();

static void uart_send_u32(uint32_t v){
```

Insert the forward declaration and dump routine so it reads:

```cpp
void spi_init();
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t length);

static void uart_send_u32(uint32_t v){
```

Then, immediately AFTER the `uart_send_u32` function's closing brace, add:

```cpp
// Stream a flash region over UART in 256-byte chunks (no large RAM buffer).
static void flash_dump_region(uint32_t addr, uint32_t length){
    uint8_t buf[256];
    for(uint32_t off = 0; off < length; off += 256){
        flash_read_data(addr + off, buf, 256);
        for(int i = 0; i < 256; i++) uart_send_byte(buf[i]);
    }
}

// Dump the raw flash image: sector 0, the allocation table, then each active
// data sector (prefixed with its big-endian address). Read-only; no FS writes.
static void flash_dump(){
    uart_send_string("DUMP:\n");
    flash_dump_region(SUPERBLOCK_ADDR, FLASH_SECTOR_SIZE);
    flash_dump_region(ALLOC_TABLE_ADDR, 3 * FLASH_SECTOR_SIZE);
    for(uint32_t i = 0; i < FLASH_TOTAL_SECTORS - 4; i++){
        AllocationEntry ae;
        flash_read_data(ALLOC_TABLE_ADDR + (i + 4) * sizeof(AllocationEntry),
                        (uint8_t*)&ae, sizeof(AllocationEntry));
        if(ae.state == SECTOR_ACTIVE){
            uint32_t sector_addr = DATA_START_ADDR + i * FLASH_SECTOR_SIZE;
            uart_send_u32(sector_addr);
            flash_dump_region(sector_addr, FLASH_SECTOR_SIZE);
        }
    }
    uart_send_string("DUMP_END:\n");
}
```

- [ ] **Step 2: Add the dump trigger before fs_init**

In `src/main.cpp`, find:

```cpp
    uart_init();
    uart_send_string("STM32 booted\n");
    spi_init();
    fs_init();
```

Replace with:

```cpp
    uart_init();
    uart_send_string("STM32 booted\n");
    spi_init();

    // Dump-mode trigger: if 0xDD arrives within ~2s of boot, stream the raw flash
    // image and halt. Runs BEFORE fs_init so it never reformats/writes the image.
    for(volatile uint32_t w = 0; w < 2000000; w++){
        if(uart_rx_ready() && uart_receive() == 0xDD){
            flash_dump();
            while(1);
        }
    }

    fs_init();
```

- [ ] **Step 3: Build**

Run: `make all`
Expected: links `flashfs.elf` with no errors. (`AllocationEntry`, `SECTOR_ACTIVE`, and the addr/sector constants come from `fs.h`; `uart_rx_ready`/`uart_receive` from `uart.h`.)

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: 0xDD-triggered raw flash dump over UART (pre-fs_init, read-only)"
```

---

### Task 5: README tools docs

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Fetch first (README is GitHub-edited)**

Run: `git fetch origin` (rebase if `main` moved).

- [ ] **Step 2: Add a flash_analyzer note after the Visualization section**

In `README.md`, immediately before `## Build and Flash`, add:

```markdown
## Flash image analyzer

`tools/flash_analyzer.py` dumps and decodes the raw on-flash image off-target. Send the board the byte `0xDD` within ~2 s of reset and it streams sector 0, the allocation table, and every active data sector over UART (read-only, before `fs_init`, so the image is untouched). The analyzer triggers this, then prints the superblock, directory, allocation-table wear stats, and per-page CRC pass/fail, and renders a wear-leveling bar chart (`docs/wear-leveling.png`).

```bash
python3 tools/flash_analyzer.py /dev/cu.usbserial-0001 115200   # then press RESET
```

## Build and Flash
```

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document flash_analyzer.py in the README"
```

---

### Task 6: Verification

**Files:** none

- [ ] **Step 1: Host checks (no hardware)**

```bash
python3 tests/test_analyzer.py            # expect: ALL TESTS PASSED
python3 -m py_compile tools/flash_analyzer.py && echo OK
make all                                  # expect: flashfs.elf links
```

- [ ] **Step 2: Hardware check**

```bash
make flash
python3 tools/flash_analyzer.py /dev/cu.usbserial-0001 115200   # press RESET when prompted
```

Expected: prints a plausible superblock (`magic=0xdeadbeef version=2`), a directory
of active files, allocation wear stats, and DATA PAGES with `PASS` CRCs for valid
pages; a wear-leveling chart window opens and `docs/wear-leveling.png` is written.

- [ ] **Step 3: Finish the branch**

Use `superpowers:finishing-a-development-branch`.

---

## Self-review notes

- **Spec coverage:** parsers + crc16 → Task 1; dump reader + report + chart → Task 2; UART RX → Task 3; firmware trigger + flash_dump → Task 4; README → Task 5; verification → Task 6.
- **Type consistency:** `struct` formats (`SB`/`DE`/`AE`/`PH`) match the spec table and `fs.h`; the firmware's `AllocationEntry`/`SECTOR_ACTIVE` and addr math match `parse_alloc_table` + the `i↔i+4` mapping; 4-byte addr is big-endian on both sides (`uart_send_u32` / `>I`).
- **Constraint check:** only the six allowed files; `flash.cpp`/`fs.cpp`/`fs.h` untouched; no Makefile change (Python tests run via `python3`).

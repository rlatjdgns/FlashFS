# STM32 Bare-Metal Flash File System

A wear-leveled, CRC-protected file system on external SPI NOR flash, written for the STM32F103C8T6 with **zero HAL, CMSIS, or stdlib** — startup code, linker script, drivers, and file system all from scratch via direct register access.

## Project Overview

A from-scratch flash file system for the STM32F103C8T6 (Cortex-M3). Every layer — reset/vector table, `.data`/`.bss` init, UART/SPI/I2C drivers, the EN25Q64 NOR command layer, and the file system itself — is hand-written against the reference manual with no vendor abstractions and no C library. It persists BME280 sensor readings to an 8 MB SPI NOR chip with wear leveling, per-page CRC integrity, and power-cycle persistence.

## Hardware

- **MCU:** STM32F103C8T6 — Cortex-M3, 64 KB flash, 20 KB SRAM. Runs on the **8 MHz HSI** (no PLL/clock-tree init).
- **SPI flash:** EN25Q64 — 8 MB (64 Mbit) SPI NOR, 4 KB sectors, 256 B pages.
- **Sensor:** Bosch BME280 (temperature), I2C address `0x76`.
- **Programmer:** ST-Link V2 (SWD) via OpenOCD.
- **Host link:** CP2102 USB-UART bridge @ 115200 baud.
- **Pin map:**
  - USART1 — PA9 (TX), PA10 (RX); TX used for logging/diagnostics
  - SPI1 — PA4 (CS, software GPIO), PA5 (SCK), PA6 (MISO), PA7 (MOSI); mode 0, fPCLK/256
  - I2C1 — PB6 (SCL), PB7 (SDA); 100 kHz standard mode

## Architecture

**Flash layout** (2048 × 4 KB sectors):

| Region | Address | Contents |
|--------|---------|----------|
| Superblock + directory | `0x000000` (sector 0) | magic `0xDEADBEEF`, format version, geometry, `DirectoryEntry` table |
| Allocation table | `0x001000` (sectors 1–3) | one `AllocationEntry` per data sector |
| Data | `0x004000` (sectors 4+) | file pages |

**Key data structures:**
- **`Superblock`** — magic + format version + geometry. `fs_init` validates magic/version and reformats if missing or stale (version is bumped on any on-flash layout change).
- **`PageHeader`** (12 B, prepended to each 256 B page) — `state`, `file_id`, `file_offset`, `data_length`, and a **CRC16** over the 244 B payload, verified on every read.
- **`DirectoryEntry`** — `filename[16]`, `file_id`, `file_size`, `firstpage_addr`, `status`. `firstpage_addr` is `0xFFFFFFFF` until the first write, so reads of an unwritten file fail cleanly.
- **`AllocationEntry`** (5 B, `__attribute__((packed))`) — per-sector `state` + `erase_count`. Packed so the table fits the 3 reserved sectors instead of spilling into data sector 0.

**Wear leveling:** `fs_find_free_sector` selects the free data sector with the **lowest `erase_count`**, incremented on each write. *(Current limitation: overwritten sectors are not reclaimed — the log fills flash monotonically over ~2044 writes.)*

**Integrity:** CRC16-CCITT (poly `0x1021`, init `0xFFFF`) per page; a mismatch makes the read fail rather than return corrupt data.

## Driver Stack

Bottom-up, each layer a thin register-level interface:

- **UART** (`uart.cpp`) — USART1 TX at 115200 (BRR=69 @ 8 MHz). Byte/string output for logging.
- **SPI** (`spi.cpp`) — SPI1 master, mode 0, software NSS on PA4, full-duplex byte transfer with bounded timeouts; drains stale RXNE around every byte.
- **Flash** (`flash.cpp`) — EN25Q64 commands: WREN (`0x06`), RDSR (`0x05`), sector-erase (`0x20`), page-program (`0x02`), read (`0x03`); 24-bit addressing, WIP-polled with a bounded timeout.
- **File system** (`fs.cpp`) — superblock/directory/allocation management, CRC-checked pages, wear-leveled allocation. API: `fs_init`, `fs_create`, `fs_write`, `fs_read`.
- **I2C** (`BME280.cpp`) — I2C1 master @ 100 kHz, bounded-timeout transactions, BUSY-before-START guard, RM0008 single-byte receive sequence.
- **BME280** (`BME280.cpp`) — sensor config + Bosch fixed-point temperature compensation; calibration read once at init.

## Demo

`main.cpp` runs a persistent sensor-logging loop:

1. Initializes UART/SPI/FS/BME280 and creates 32 per-slot files (`file_id` 0–31).
2. **Each iteration** (~24 s apart — an uncalibrated busy-wait delay, not a timer): reads the BME280 temperature, writes a `SensorReading {seq, temp in units of 0.01°C}` to slot `seq % 32`, and streams a live `seq + temp` record (big-endian `uint32` + `int32`) over UART.
3. **Every 5th iteration:** wraps a readback of all 32 slots in `READBACK:` / `END` markers, streaming each stored `seq + temp` record (CRC-verified) — proving round-trip integrity.
4. **Power-cycle persistence:** on boot `fs_init` finds the existing superblock and reuses the directory, so prior readings are immediately readable.

`tools/listen.py` decodes the mixed text/binary UART stream on the host.

## Build and Flash

Toolchain: `arm-none-eabi-g++`, OpenOCD, ST-Link V2.

```
make             # build flashfs.elf
make flash       # program via OpenOCD / ST-Link
make erase_flash # mass-erase MCU flash, then program
make test        # host unit tests (RAM-backed flash stub — no hardware needed)
```

## Key Technical Challenges

- **SPI full-duplex stale RXNE** — draining the shifted-in byte after every transmit to keep reads in sync.
- **NOR write model** — erase-before-write, since NOR clears bits only 1→0.
- **I2C single-byte receive (EV6_1)** — the exact ACK/STOP ordering a 1-byte read requires per RM0008.
- **Brownout during sector erase** — the erase current spike sags the 3.3 V rail; surfaced with bounded timeouts.
- **Root-causing implausible readings** — traced wild temperatures to an uninitialized readback buffer, not a failed I2C read.

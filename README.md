# STM32 Bare-Metal Flash File System
A bare- metal flash file system for STM32 Blue Pill from scratch in C++, building up from low-level hardware drivers to a working filesystem layer with zero external dependencies for firmware.

## Project Overview

A from-scratch flash file system for the STM32F103C8T6 (Cortex-M3). Every layer — reset/vector table, `.data`/`.bss` init, UART/SPI/I2C drivers, the EN25Q64 NOR command layer, and the file system itself — is hand-written against the reference manual with no vendor abstractions and no C library. It persists BME280 sensor readings to an 8 MB SPI NOR chip with dynamic wear leveling, per-page CRC integrity, and power-cycle persistence.

## Hardware
- STM32
- EN25Q64 SPI NOR Flash (8MB)
- Bosch BME280
- CP2102 USB-UART Adapter
- ST-Link V2 Programmer

**Pin map:**
- USART1 — PA9 (TX), PA10 (RX)
- SPI1 — PA4 (CS, software GPIO), PA5 (SCK), PA6 (MISO), PA7 (MOSI)
- I2C1 — PB6 (SCL), PB7 (SDA)

## Hardware Architecture
![Hardware Architecture](docs/architecture.png)
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

**Wear leveling:** `fs_find_free_sector` selects the free data sector with the lowest `erase_count`, incremented on each write. **Greedy least-erased-first (dynamic wear leveling)**
*(Current limitation: overwritten sectors are not reclaimed — the log fills flash monotonically over ~2044 writes.)*

## Driver Stack

Bottom-up, each layer a thin register-level interface:

- **UART** (`uart.cpp`) — USART1 TX at 115200 (BRR=69 @ 8 MHz). Byte/string output for logging.
- **SPI** (`spi.cpp`) — SPI1 master, mode 0, software NSS on PA4, full-duplex byte transfer with bounded timeouts; drains stale RXNE around every byte.
- **Flash** (`flash.cpp`) — EN25Q64 commands: WREN (`0x06`), RDSR (`0x05`), sector-erase (`0x20`), page-program (`0x02`), read (`0x03`); 24-bit addressing, WIP-polled with a bounded timeout.
- **File system** (`fs.cpp`) — superblock/directory/allocation management, CRC-checked pages, wear-leveled allocation. API: `fs_init`, `fs_create`, `fs_write`, `fs_read`.
- **I2C** (`BME280.cpp`) — I2C1 master @ 100 kHz, bounded-timeout transactions, BUSY-before-START guard, RM0008 single-byte receive sequence.
- **BME280** (`BME280.cpp`) — sensor config + Bosch fixed-point temperature compensation; calibration read once at init.

## Protocol

`main.cpp` runs a persistent sensor-logging loop:

1. Initializes UART/SPI/FS/BME280 and creates 32 per-slot files (`file_id` 0–31).
2. **Each iteration** (~24 s apart — an uncalibrated busy-wait delay, not a timer): reads the BME280 temperature, writes a `SensorReading {seq, temp in units of 0.01°C}` to slot `seq % 32`, and streams a live `seq + temp` record (big-endian `uint32` + `int32`) over UART.
3. **Every 5th iteration:** wraps a readback of all 32 slots in `READBACK:` / `END` markers, streaming each stored `seq + temp` record (CRC-verified) — proving round-trip integrity.
4. **Power-cycle persistence:** on boot `fs_init` finds the existing superblock and reuses the directory, so prior readings are immediately readable.

`tools/listen.py` decodes the mixed text/binary UART stream on the host.

## Key Technical Challenges
### SPI full-duplex stale RXNE
- **Symptom:** JEDEC ID returned plausible-looking but wrong bytes, and every subsequent read returned data from the *previous* transaction — reads were offset by one transfer, not corrupted.
- **How it was found:** Bit-banged the same command sequence on GPIO and compared against the hardware SPI path. The bit-banged version read correctly, isolating the fault to the peripheral's data register rather than wiring or the flash part.
- **The fix:** SPI is inherently full-duplex — every byte shifted out shifts one in. `spi_transmit()` now drains RXNE both before transmitting (clearing any stale byte) and after (discarding the garbage byte clocked in alongside the command), keeping the receive FIFO aligned with the transaction.

### Brownout during sector erase
- **Symptom:** Erases intermittently hung or completed with corrupt data,
  clustered around multi-sector operations. Non-deterministic across otherwise
  identical runs.
- **How it was found:** Replacing infinite `while` polling loops with bounded
  timeouts converted silent hangs into reported failures, which localized the
  fault to `flash_wait_busy()` after `flash_sector_erase()`.
- **The fix:** Erase draws a current spike large enough to sag a 3.3 V rail
  shared between the CP2102 and the STM32. Moving each device to a separate host
  USB port eliminated the failures across 16,000 subsequent writes.

### I2C single-byte receive sequence (EV6_1)
- **Symptom:** BME280 single-byte register reads returned stale or shifted data; the chip-ID read would not settle at 0x60.
- **How it was found:** Traced the transfer against the EV6_1 event sequence in RM0008 §26.3.3 and compared each step against the peripheral status registers.
- **The fix:** A 1-byte read requires a specific order: clear ACK, read SR2 to clear ADDR, set STOP, *then* wait on RXNE. Performing these out of order lets the peripheral ACK a byte that was never requested. Corrected ordering yielded a stable 0x60 and ~27.43 °C.

### Implausible sensor readings from an uninitialized buffer
- **Symptom:** Temperature readings swung wildly between runs while the I2C transaction itself reported success.
- **How it was found:** The failure pointed at I2C by default. Dumping the raw readback buffer before compensation showed the transport was fine — the garbage was already present in the destination buffer.
- **The fix:** With no stdlib, the buffer was never zeroed and the read left upper bytes untouched. Explicit byte-loop initialization before every readback. Confirmed the class of bug was transport-independent, not I2C-specific.

## Visualization
`tools/visualize.py` plots the UART stream live with matplotlib. The **blue line** is the temperature the BME280 measures each loop and the **orange dots** are readings the firmware reads back out of flash every 5th loop, each placed at its stored sequence number. Dots tracking the line confirm the file system stores and returns data intact. 

<p align="center">
  <img src="docs/visualize-demo.png" width="50%">
</p>  


## Flash image analyzer
`tools/flash_analyzer.py` dumps and decodes the raw on-flash image off-target and decodes it. The analyzer triggers this, then prints the superblock, directory, allocation-table wear stats, and per-page CRC pass/fail, and renders a wear-leveling bar chart. The bar chart charts erase count of every sector to show the allocator spreading wear evenly. 

<p align="center">
  <img src="docs/wear-leveling-early.png" width="45%">
  <img src="docs/wear-leveling-full.png" width="45%">
</p>

```bash
python3 tools/visualize.py /dev/cu.usbserial-0001 115200       # live temperature plot
python3 tools/flash_analyzer.py /dev/cu.usbserial-0001 115200  # wear chart (then press RESET)
```

## Build and Flash

Toolchain: `arm-none-eabi-g++`, OpenOCD, ST-Link V2.

```
make             # build flashfs.elf
make flash       # program via OpenOCD / ST-Link
make erase_flash # mass-erase MCU flash, then program
make test        # host unit tests (RAM-backed flash stub — no hardware needed)
```

## Related
**[TACTNET](https://github.com/rlatjdgns/Tactnet)** — 3-node LoRa mesh network in C++ with custom UART/I2C drivers, priority task scheduler, and timestamp-based failover.

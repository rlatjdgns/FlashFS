# STM32 Bare-Metal Flash File System
A baremental flash file system for STM32 Blue Pill from scratch in C++, uilding up from low-level hardware drivers to a working filesystem layer with zero external dependencies.

## Project Overview

A from-scratch flash file system for the STM32F103C8T6 (Cortex-M3). Every layer — reset/vector table, `.data`/`.bss` init, UART/SPI/I2C drivers, the EN25Q64 NOR command layer, and the file system itself — is hand-written against the reference manual with no vendor abstractions and no C library. It persists BME280 sensor readings to an 8 MB SPI NOR chip with wear leveling, per-page CRC integrity, and power-cycle persistence.

## Hardware
| Component | Quantity |
|-----------|----------|
| STM32 | 1 |
| EN25Q64 SPI NOR Flash (8MB) | 1 |
| Bosch BME280| 1 |
| CP2102 USB-UART Adapter | 1 |
| ST-Link V2 Programmer | 1 |

## Wiring 


## Architecture
![Hardware Architecture Diagram](docs/architecture.png)
**Flash layout** (2048 × 4 KB sectors):

| Region | Address | Contents |
|--------|---------|----------|
| Superblock + directory | `0x000000` (sector 0) | magic `0xDEADBEEF`, format version, geometry, `DirectoryEntry` table |
| Allocation table | `0x001000` (sectors 1–3) | one `AllocationEntry` per data sector |
| Data | `0x004000` (sectors 4+) | file pages |

## Software Architecture 

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

## Visualization

`tools/visualize.py` plots the UART stream live with matplotlib. The **blue line** is the temperature the BME280 measures each loop; the **orange dots** are readings the firmware reads back out of flash every 5th loop, each placed at its stored sequence number. Dots tracking the line confirm the file system stores and returns data intact. The capture below shows the last 32 stored readings as a rising history with live points at the leading edge — real data over UART from the board.

![Live temperature plot — blue live-sensor line and orange flash-readback dots](docs/visualize-demo.png)

```bash
python3 tools/visualize.py /dev/cu.usbserial-0001 115200
```

## Flash image analyzer

`tools/flash_analyzer.py` dumps and decodes the raw on-flash image off-target. Send the board the byte `0xDD` within ~2 s of reset and it streams sector 0, the allocation table, and every active data sector over UART (read-only, before `fs_init`, so the image is untouched). The analyzer triggers this, then prints the superblock, directory, allocation-table wear stats, and per-page CRC pass/fail, and renders a wear-leveling bar chart (`docs/wear-leveling.png`).

```bash
python3 tools/flash_analyzer.py /dev/cu.usbserial-0001 115200   # then press RESET
```

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

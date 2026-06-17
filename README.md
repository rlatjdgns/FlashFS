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

**EN25Q64 SPI NOR Flash → STM32 (SPI1):**
* CS → PA4 (NSS, chip select)
* CLK → PA5 (SCK)
* DO → PA6 (MISO)
* DI → PA7 (MOSI)
* VCC → 3.3V
* GND → GND

**BME280 → STM32 (I2C1, 100kHz):**
* SCL → PB6 (I2C clock)
* SDA → PB7 (I2C data)
* VCC → 3.3V
* GND → GND

**CP2102 USB-UART → STM32 (USART1, 115200 baud):**
* RXD → PA9 (STM32 TX)
* TXD → PA10 (STM32 RX)
* GND → GND

## Hardware Architecture
![Hardware Architecture](docs/architecture.png)
**Flash layout** (2048 × 4 KB sectors):

| Region | Address | Contents |
|--------|---------|----------|
| Superblock + directory | `0x000000` (sector 0) | magic `0xDEADBEEF`, format version, geometry, `DirectoryEntry` table |
| Allocation table | `0x001000` (sectors 1–3) | one `AllocationEntry` per data sector |
| Data | `0x004000` (sectors 4+) | file pages |

## Software Architecture 

Bottom-up, each layer a thin register-level interface:

**uart.cpp** — Sets up USART1 and sends bytes/strings over serial at 115200 baud. This is how the board talks to the computer, used for all logging and debug output.
**spi.cpp** — Drives SPI1 as master to talk to the flash chip. Handles full-duplex byte transfers and makes sure stale data gets drained off the receive register around every byte so reads don't come back garbage.
**flash.cpp** — Sends the EN25Q64's command set over SPI: write enable, read status, sector erase, page program, and read. Handles 24-bit addressing and waits for the chip to finish each operation before moving on.
**fs.cpp** — The actual file system. Manages the superblock, directory, and allocation table, and exposes the main API: fs_init, fs_create, fs_write, fs_read. Pages are CRC-checked and writes are spread across sectors for wear leveling.
**BME280.cpp** — Sets up I2C1 to talk to the BME280 sensor, then handles reading temperature and running it through Bosch's compensation math. Calibration data is read once at startup.

## Demo

`main.cpp` runs a persistent sensor-logging loop:

1. Initializes UART/SPI/FS/BME280 and creates 32 per-slot files (`file_id` 0–31).
2. **Each iteration** (~24 s apart — an uncalibrated busy-wait delay, not a timer): reads the BME280 temperature, writes a `SensorReading {seq, temp in units of 0.01°C}` to slot `seq % 32`, and streams a live `seq + temp` record (big-endian `uint32` + `int32`) over UART.
3. **Every 5th iteration:** wraps a readback of all 32 slots in `READBACK:` / `END` markers, streaming each stored `seq + temp` record (CRC-verified) — proving round-trip integrity.
4. **Power-cycle persistence:** on boot `fs_init` finds the existing superblock and reuses the directory, so prior readings are immediately readable.

`tools/listen.py` decodes the mixed text/binary UART stream on the host.

## Visualization

`tools/visualize.py` plots the UART stream live with matplotlib. The **blue line** is the temperature the BME280 measures each loop; the **orange dots** are readings the firmware reads back out of flash every 5th loop, each placed at its stored sequence number. Dots tracking the line confirm the file system stores and returns data intact. The capture below shows the last 32 stored readings as a rising history with live points at the leading edge — real data over UART from the board.

<p align="center">
  <img src="docs/visualize-demo.png" width="50%">
</p>  
<p align="center">
  <img src="docs/wear-leveling-early.png" width="45%">
  <img src="docs/wear-leveling-full.png" width="45%">
</p>

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

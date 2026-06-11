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

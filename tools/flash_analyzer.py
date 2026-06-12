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

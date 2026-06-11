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

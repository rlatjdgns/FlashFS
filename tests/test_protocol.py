import struct, sys, os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from stm_protocol import feed

REC = struct.Struct(">Ii")   # big-endian uint32 seq, int32 temp (0.01 C)

failures = 0
def check(cond, msg):
    global failures
    if not cond:
        print(f"FAIL: {msg}")
        failures += 1

def check_event(ev, kind, seq, celsius):
    ok = ev[0] == kind and ev[1] == seq and abs(ev[2] - celsius) < 1e-9
    check(ok, f"expected ({kind}, {seq}, {celsius}) got {ev}")

def test_live_and_readback():
    state = {"in_readback": False}
    buf = bytearray()
    buf += b"STM32 booted\n"
    buf += REC.pack(0, 2805)          # live, 28.05 C
    buf += b"READBACK:\n"
    buf += REC.pack(0, 2805)          # readback
    buf += REC.pack(32, 2810)         # readback
    buf += b"END\n"
    ev = feed(buf, state)
    check(ev[0] == ("text", "STM32 booted"), f"boot text: {ev[0]}")
    check_event(ev[1], "live", 0, 28.05)
    check(ev[2] == ("text", "READBACK:"), f"readback marker: {ev[2]}")
    check_event(ev[3], "readback", 0, 28.05)
    check_event(ev[4], "readback", 32, 28.10)
    check(ev[5] == ("text", "END"), f"end marker: {ev[5]}")
    check(state["in_readback"] is False, "mode reset after END")
    check(len(buf) == 0, "buffer fully consumed")

def test_split_record_across_feeds():
    state = {"in_readback": False}
    buf = bytearray()
    rec = REC.pack(7, -150)           # -1.50 C
    buf += rec[:3]                    # first 3 of 8 bytes arrive
    check(feed(buf, state) == [], "no events on partial record")
    check(len(buf) == 3, "partial record retained")
    buf += rec[3:]                    # remaining 5 bytes arrive
    ev = feed(buf, state)
    check(len(ev) == 1, f"one event after completion: {ev}")
    check_event(ev[0], "live", 7, -1.5)
    check(len(buf) == 0, "buffer consumed after completion")

test_live_and_readback()
test_split_record_across_feeds()
if failures == 0:
    print("ALL TESTS PASSED")
    sys.exit(0)
print(f"{failures} CHECK(s) FAILED")
sys.exit(1)

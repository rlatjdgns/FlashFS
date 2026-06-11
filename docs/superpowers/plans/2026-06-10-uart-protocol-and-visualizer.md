# UART Protocol + Real-Time Visualizer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Emit a framed UART protocol (8-byte `seq+temp` records + `READBACK:`/`END` markers) from the firmware and plot live vs flash-readback temperature in real time, with a shared decoder used by both the plotter and the existing listener.

**Architecture:** One pure Python decoder (`tools/stm_protocol.py`) frames the mixed text/binary stream; `visualize.py` (matplotlib) and `listen.py` both consume it. The firmware (`src/main.cpp`) emits the protocol. No FS logic or on-flash format changes.

**Tech Stack:** C++ bare-metal (`arm-none-eabi-g++`), Python 3 + pyserial + matplotlib.

**Design spec:** `docs/superpowers/specs/2026-06-10-uart-protocol-and-visualizer-design.md`

**Constraint:** Touch ONLY `src/main.cpp`, `tools/stm_protocol.py`, `tools/visualize.py`, `tools/listen.py`, `tests/test_protocol.py`, `README.md`. Do not modify FS logic, the on-flash format, or the Makefile.

---

## Pre-flight

On `main`; branch before the first commit:

```bash
git checkout -b feat-uart-protocol-visualizer
```

---

## File structure

- Create `tools/stm_protocol.py` — pure stream decoder (`feed(buf, state) -> events`).
- Create `tests/test_protocol.py` — standalone asserts for the decoder.
- Create `tools/visualize.py` — matplotlib real-time plot (procedural).
- Modify `tools/listen.py` — swap its decode core for `stm_protocol.feed`.
- Modify `src/main.cpp` — emit the framed protocol.
- Modify `README.md` — update the Demo lines to the new protocol.

---

### Task 1: Shared decoder + failing test

**Files:**
- Create: `tools/stm_protocol.py`
- Create: `tests/test_protocol.py`

- [ ] **Step 1: Write the failing test**

Create `tests/test_protocol.py`:

```python
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
```

- [ ] **Step 2: Run the test to verify it FAILS**

Run: `python3 tests/test_protocol.py`
Expected: `ModuleNotFoundError: No module named 'stm_protocol'` (the module does not exist yet).

- [ ] **Step 3: Write the decoder**

Create `tools/stm_protocol.py`:

```python
"""Decoder for the STM32 flash-FS UART stream.

The firmware mixes printable text lines (terminated by '\\n') with 8-byte binary
records. A record is uint32 seq + int32 temp (big-endian); temp is in units of
0.01 C. Records between the text lines 'READBACK:' and 'END' are flash readback;
all others are live.

Framing: seq stays below 2**24 for any realistic run, so a record's leading byte
(seq's high byte) is always 0x00 (non-printable) and never collides with a
printable text line's leading byte.
"""

import struct

_RECORD = struct.Struct(">Ii")   # big-endian uint32 seq, int32 temp


def _printable(b):
    return 0x20 <= b <= 0x7E or b in (0x09, 0x0A, 0x0D)


def feed(buf, state):
    """Consume complete lines/records from `buf` (a bytearray, mutated in place).

    Returns a list of events:
      ("live", seq, celsius)
      ("readback", seq, celsius)
      ("text", line)
    Leaves any incomplete trailing line/record in `buf` for the next call.
    `state` is a dict; feed() reads and sets state["in_readback"] (bool).
    """
    events = []
    while buf:
        if _printable(buf[0]):
            nl = buf.find(b"\n")
            if nl == -1:
                break                       # incomplete text line; wait for more
            line = bytes(buf[:nl]).rstrip(b"\r").decode("ascii", "replace")
            del buf[:nl + 1]
            if line == "READBACK:":
                state["in_readback"] = True
            elif line == "END":
                state["in_readback"] = False
            events.append(("text", line))
        else:
            if len(buf) < _RECORD.size:
                break                       # incomplete record; wait for more
            seq, temp = _RECORD.unpack_from(buf)
            del buf[:_RECORD.size]
            kind = "readback" if state.get("in_readback") else "live"
            events.append((kind, seq, temp / 100.0))
    return events
```

- [ ] **Step 4: Run the test to verify it PASSES**

Run: `python3 tests/test_protocol.py`
Expected: `ALL TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tools/stm_protocol.py tests/test_protocol.py
git commit -m "feat: shared UART stream decoder with unit tests"
```

---

### Task 2: Real-time visualizer

**Files:**
- Create: `tools/visualize.py`

- [ ] **Step 1: Write the visualizer**

Create `tools/visualize.py`:

```python
#!/usr/bin/env python3
"""Real-time BME280 temperature plot from the STM32 flash-FS UART stream.

Usage: python3 tools/visualize.py /dev/tty.usbserial-0001 115200
"""
import sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stm_protocol import feed

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed. Run: pip install pyserial")
try:
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib not installed. Run: pip install matplotlib")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: python3 visualize.py <port> <baud>")
    port, baud = sys.argv[1], int(sys.argv[2])

    ser = serial.Serial(port, baud, timeout=0.05)
    print(f"# reading {port} @ {baud} baud (close the window to quit)", file=sys.stderr)

    live_x, live_y, rb_x, rb_y = [], [], [], []
    buf = bytearray()
    state = {"in_readback": False}

    plt.ion()
    fig, ax = plt.subplots()
    live_line, = ax.plot([], [], "b-", label="Live sensor")
    rb_dots, = ax.plot([], [], "o", color="orange", label="Flash readback")
    ax.set_title("BME280 Temperature Log — STM32 Flash FS")
    ax.set_xlabel("Reading #")
    ax.set_ylabel("Temperature (°C)")
    ax.legend()
    fig.show()

    while plt.fignum_exists(fig.number):
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            for ev in feed(buf, state):
                if ev[0] == "live":
                    live_x.append(ev[1]); live_y.append(ev[2])
                elif ev[0] == "readback":
                    rb_x.append(ev[1]); rb_y.append(ev[2])
            live_line.set_data(live_x, live_y)
            rb_dots.set_data(rb_x, rb_y)
            ax.relim(); ax.autoscale_view()
        plt.pause(0.05)

    ser.close()


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Verify it compiles (no hardware needed)**

Run: `python3 -m py_compile tools/visualize.py && echo OK`
Expected: `OK` (full behavior is verified on hardware in Task 6).

- [ ] **Step 3: Commit**

```bash
git add tools/visualize.py
git commit -m "feat: real-time matplotlib temperature visualizer"
```

---

### Task 3: Update listen.py to the new protocol

**Files:**
- Modify: `tools/listen.py`

- [ ] **Step 1: Add the shared decoder import**

In `tools/listen.py`, just after `import time`, add:

```python
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stm_protocol import feed
```

- [ ] **Step 2: Remove the old decode functions**

Delete the `is_text_byte`, `decode_temp`, and `parse` functions from `tools/listen.py`
(they are replaced by `feed`). Keep `render_text` — it is still used by `--raw`.
Also remove the `--no-decode` argument definition (the line adding `dest="decode"`),
since decoding is now unconditional.

- [ ] **Step 3: Rewrite the decode branch of the main loop**

In `tools/listen.py`, replace this block in `main()`:

```python
            buf.extend(chunk)
            for kind, text in parse(buf, args.decode):
                prefix = stamp() + ("  " if kind == "temp" else "")
                print(f"{prefix}{text}")
            sys.stdout.flush()
```

with:

```python
            buf.extend(chunk)
            for ev in feed(buf, state):
                if ev[0] == "text":
                    print(f"{stamp()}{ev[1]}")
                else:
                    label = "LIVE" if ev[0] == "live" else "RB  "
                    print(f"{stamp()}  {label}  #{ev[1]}  {ev[2]:.2f} C")
            sys.stdout.flush()
```

And initialize the decoder state next to `buf = bytearray()` in `main()`:

```python
    buf = bytearray()
    state = {"in_readback": False}
```

- [ ] **Step 4: Verify it compiles**

Run: `python3 -m py_compile tools/listen.py && echo OK`
Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add tools/listen.py
git commit -m "refactor(listen): decode the framed seq+temp protocol via shared decoder"
```

---

### Task 4: Emit the protocol from firmware

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Add the big-endian u32 helper**

In `src/main.cpp`, after the `void spi_init();` forward declaration and before `int main()`, add:

```cpp
static void uart_send_u32(uint32_t v){
    uart_send_byte((v >> 24) & 0xFF);
    uart_send_byte((v >> 16) & 0xFF);
    uart_send_byte((v >> 8)  & 0xFF);
    uart_send_byte((v >> 0)  & 0xFF);
}
```

- [ ] **Step 2: Replace the live-send and readback blocks**

In `src/main.cpp`, replace this block:

```cpp
        uart_send_byte((T >> 24) & 0xFF);
        uart_send_byte((T >> 16) & 0xFF);
        uart_send_byte((T >> 8) & 0xFF);
        uart_send_byte((T >> 0) & 0xFF);
        if(seq > 0 && seq % 5 == 0){
            for(uint8_t fid = 0; fid < MAX_FILES; fid++){
                SensorReading r;
                r.seq = 0;
                r.temp = 0;
                if(fs_read(fid, (uint8_t*)&r, sizeof(SensorReading)) == 0){
                    uart_send_byte((r.temp >> 24) & 0xFF);
                    uart_send_byte((r.temp >> 16) & 0xFF);
                    uart_send_byte((r.temp >> 8) & 0xFF);
                    uart_send_byte((r.temp >> 0) & 0xFF);
                }
            }
        }
```

with:

```cpp
        uart_send_u32(seq);
        uart_send_u32((uint32_t)T);
        if(seq > 0 && seq % 5 == 0){
            uart_send_string("READBACK:\n");
            for(uint8_t fid = 0; fid < MAX_FILES; fid++){
                SensorReading r;
                r.seq = 0;
                r.temp = 0;
                if(fs_read(fid, (uint8_t*)&r, sizeof(SensorReading)) == 0){
                    uart_send_u32(r.seq);
                    uart_send_u32((uint32_t)r.temp);
                }
            }
            uart_send_string("END\n");
        }
```

- [ ] **Step 3: Build the firmware**

Run: `make all`
Expected: links `flashfs.elf` with no errors. (`uart_send_string` is already declared via `uart.h`.)

- [ ] **Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat: emit framed seq+temp UART protocol with READBACK/END markers"
```

---

### Task 5: Update README Demo section

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Fetch first (README is edited on GitHub)**

Run: `git fetch origin` (and rebase if `main` moved: `git pull --rebase origin main`).

- [ ] **Step 2: Update the two Demo lines**

In `README.md`, replace Demo list item 2's tail
"…and streams the live value over UART as a big-endian `int32`."
and item 3 with:

```markdown
2. **Each iteration** (~24 s apart — an uncalibrated busy-wait delay, not a timer): reads the BME280 temperature, writes a `SensorReading {seq, temp in units of 0.01°C}` to slot `seq % 32`, and streams a live `seq + temp` record (big-endian `uint32` + `int32`) over UART.
3. **Every 5th iteration:** wraps a readback of all 32 slots in `READBACK:` / `END` markers, streaming each stored `seq + temp` record (CRC-verified) — proving round-trip integrity.
```

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: update Demo section for the framed UART protocol"
```

---

### Task 6: Verification

**Files:** none (verification only)

- [ ] **Step 1: Host decoder tests + compile checks (no hardware)**

```bash
python3 tests/test_protocol.py            # expect: ALL TESTS PASSED
python3 -m py_compile tools/stm_protocol.py tools/visualize.py tools/listen.py && echo OK
make all                                  # expect: flashfs.elf links cleanly
```

- [ ] **Step 2: Hardware check**

Flash and observe (close any other listener — the port is single-access):

```bash
make flash
python3 tools/visualize.py /dev/cu.usbserial-0001 115200
```

Expected: the blue "Live sensor" line grows in real time; every 5th reading,
orange "Flash readback" dots appear at their stored sequence numbers (landing on
the live line). Then `python3 tools/listen.py /dev/cu.usbserial-0001` prints
labeled `LIVE #n` / `RB #n` lines and the `READBACK:` / `END` markers, with no
garbage.

- [ ] **Step 3: Finish the branch**

Use `superpowers:finishing-a-development-branch` to merge/PR per preference.

---

## Self-review notes

- **Spec coverage:** wire protocol → Task 4 (firmware) + Task 1 (decoder); `stm_protocol.py` → Task 1; `visualize.py` → Task 2; `listen.py` update → Task 3; `test_protocol.py` → Task 1; README touch-up → Task 5; verification criteria → Task 6.
- **Type consistency:** `feed(buf, state)` returns `("live"|"readback", seq, celsius)` / `("text", line)` — consumed identically in `visualize.py` (Task 2) and `listen.py` (Task 3); `state = {"in_readback": bool}` used everywhere. Firmware record layout `uint32 seq + int32 temp` matches the decoder's `struct ">Ii"`.
- **Constraint check:** only the six allowed files are touched; no Makefile target added (Python test runs via `python3` directly); no FS/on-flash changes.

# UART Protocol + Real-Time Temperature Visualizer — Design

Date: 2026-06-10
Status: Approved (design)

## Problem

The demo streams temperatures over UART, but the current stream is a flat run of
4-byte temps with no sequence numbers and no markers separating **live** readings
from **flash readback** values. A real-time plot that distinguishes the two (blue
live line + orange readback dots, positioned at their true sequence number) is
impossible against that stream.

## Goal

Define a small framed UART protocol carrying sequence numbers and a readback
delimiter, emit it from the firmware, and provide a procedural matplotlib script
that plots live vs readback temperature in real time. Update the existing
`listen.py` to the same protocol, and share one decoder between both tools.

## Wire protocol (the shared contract)

All multi-byte values are **big-endian**.

- **Text control lines** — printable ASCII terminated by `\n`: `STM32 booted`,
  `READBACK:`, `END`. Decoder rule: if the next unconsumed byte is printable
  (`0x20`–`0x7E`) or whitespace, accumulate to `\n` and emit a text line.
- **Binary record** — 8 bytes: `uint32 seq` then `int32 temp` (units of 0.01 °C).
  Decoder rule: if the next byte is non-printable, consume exactly 8 bytes.
- **Readback mode** — records emitted between `READBACK:` and `END` are flash
  readback; all other records are live.

**Framing safety:** `seq` stays below 2^24 for any realistic run (2^24 readings ×
~24 s ≈ 12 years), so a record's leading byte (seq's high byte) is always `0x00` —
non-printable — and never collides with a text line's printable leading byte. This
assumption is documented in the decoder.

## Components

### 1. Firmware — `src/main.cpp`

Add a big-endian helper:

```c
void uart_send_u32(uint32_t v){
    uart_send_byte((v >> 24) & 0xFF);
    uart_send_byte((v >> 16) & 0xFF);
    uart_send_byte((v >> 8)  & 0xFF);
    uart_send_byte((v >> 0)  & 0xFF);
}
```

Per loop iteration:
1. (unchanged) read temp, build `reading{seq, temp}`, `fs_write(seq % MAX_FILES, …)`.
2. **Live record:** `uart_send_u32(seq); uart_send_u32((uint32_t)T);`
3. **Every 5th iteration** (`seq > 0 && seq % 5 == 0`): send `"READBACK:\n"`; for
   each slot `fid` where `fs_read(fid, &r) == 0`, send `uart_send_u32(r.seq);
   uart_send_u32((uint32_t)r.temp);`; then send `"END\n"`.

`"STM32 booted\n"` stays. The SPI/FLASH fault diagnostics stay (they are printable
text lines and the decoder ignores unknown text).

### 2. Shared decoder — `tools/stm_protocol.py` (new)

One pure function, no classes, no I/O, no heavy imports:

```python
def feed(buf: bytearray, state: dict) -> list[tuple]:
    """Consume complete lines/records from buf (mutating it); return events.
    Events: ("live", seq, celsius) | ("readback", seq, celsius) | ("text", line)
    state: {"in_readback": bool}. Leaves an incomplete tail in buf for next call.
    """
```

Toggles `state["in_readback"]` on `READBACK:` / `END`. Routes records to `live`
or `readback` by that flag. Divides temp by 100.0 for °C.

### 3. Visualizer — `tools/visualize.py` (new)

Procedural, no classes:
- `sys.argv[1]` = port, `sys.argv[2]` = baud → `serial.Serial(port, baud, timeout=0.05)`.
- `matplotlib` interactive mode (`plt.ion()`): blue line `'b-'` labeled
  "Live sensor" (x = seq, y = °C); orange dots `'o'` labeled "Flash readback".
  Title `"BME280 Temperature Log — STM32 Flash FS"`, x-label `"Reading #"`,
  y-label `"Temperature (°C)"`, legend.
- Loop: read available bytes → `feed()` → append to live/readback lists →
  `set_data` → `relim`/`autoscale_view` → `plt.pause(0.05)`; exit when the window
  closes.

### 4. `listen.py` update — `tools/listen.py`

Replace its 4-byte decode core with `stm_protocol.feed`, printing `live`/`readback`
events as labeled lines and text lines as-is. Keep existing CLI features
(port auto-detect, `--raw`, `--timestamp`, `--list`).

### 5. Test — `tests/test_protocol.py` (new)

Standalone (assert-based, no hardware/matplotlib/serial): feed a synthetic stream —
`"STM32 booted\n"`, a live record, a `READBACK:` block with two records and `END`,
**including a record split across two `feed()` calls** — and assert the emitted
events (types, seq, °C) and that no bytes are lost across the split.

### 6. README touch-up — `README.md`

The Demo section currently describes the old 4-byte stream. Update the two lines to
the framed protocol: live readings are `seq+temp` records; the readback burst is
wrapped in `READBACK:` / `END`. One- or two-line edit; no other sections change.

## Data flow

```
firmware UART --> bytes --> serial.read --> bytearray buf
   --> stm_protocol.feed(buf, state) --> events
        ("live", seq, c)     --> live_x/live_y     --> blue line
        ("readback", seq, c) --> rb_x/rb_y         --> orange dots
        ("text", line)       --> ignored (or printed by listen.py)
```

## Error handling

- **Partial reads:** `feed` consumes only complete lines/records; an incomplete
  tail stays in `buf` for the next call.
- **Unknown text** (boot banner, fault diagnostics): emitted as `("text", …)`;
  the visualizer ignores it, `listen.py` prints it.
- **Malformed/`END` without `READBACK:`:** treated as a no-op mode toggle; never
  raises.

## Out of scope

- No change to the on-flash format or FS logic.
- No persistence/export of plotted data (live view only).
- No multi-metric plotting (temperature only; pressure/humidity unimplemented).

## Verification / success criteria

1. `python3 tests/test_protocol.py` → all asserts pass (deterministic, no hardware).
2. On hardware: flash firmware, run `python3 tools/visualize.py <port> 115200`;
   the blue live line grows in real time and orange readback dots land on it every
   5th reading. `listen.py` prints labeled live/readback lines without garbage.

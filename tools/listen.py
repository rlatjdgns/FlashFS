#!/usr/bin/env python3
"""
Serial listener for the stm32-flashfs board.

The firmware mixes two kinds of output on the same UART:
  * human-readable trace tags ending in '\\n'   (e.g. "I2C:to R-ADDRW", "SR1=0000")
  * raw 4-byte temperatures                      (T as big-endian int32, no delimiter)

A naive text decoder chokes on the binary temps, and worse, a temp like 0x00000ABC
contains 0x0A ('\\n') which a line-splitter mistakes for a line break.

Disambiguation (reliable for this firmware): every text byte is printable ASCII
(or \\t \\r \\n); every temperature's leading byte is non-printable (0x00 for
positive, 0xFF for negative, and the same for failed-read garbage 0xFFFFC91D /
0x00000000). So: a printable byte begins a text line; a non-printable byte begins
a 4-byte temperature. The 0x0A *inside* a temp is consumed as part of the 4-byte
group, never seen as a newline.

Temps are decoded as signed big-endian int32 in units of 0.01 C (BME280 t_fine math).

Baud is 115200 (BRR=69 @ 8 MHz HSI).

Usage:
    python3 tools/listen.py                       # auto-detect port, decode temps
    python3 tools/listen.py /dev/cu.usbserial-0001
    python3 tools/listen.py -t                     # timestamp every line
    python3 tools/listen.py --no-decode            # show temps as [HH] bytes, don't interpret
    python3 tools/listen.py --raw                  # dump every byte as it arrives
    python3 tools/listen.py --list                 # list ports and exit
"""

import argparse
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed.  Run:  pip install pyserial")


def find_port():
    """Pick the most likely USB-serial / ST-Link VCP port (prefer macOS cu.*)."""
    candidates = list(list_ports.comports())
    if not candidates:
        return None
    preferred = ("usbserial", "usbmodem", "ttyUSB", "ttyACM", "SLAB", "wchusb")
    matches = [p.device for p in candidates
               if any(k.lower() in p.device.lower() for k in preferred)]
    # On macOS prefer the call-out (cu.*) device — it won't block on carrier detect.
    for d in matches:
        if "cu." in d:
            return d
    return matches[0] if matches else candidates[0].device


def is_text_byte(b: int) -> bool:
    """True for printable ASCII and the whitespace the firmware actually emits."""
    return 0x20 <= b < 0x7F or b in (0x09, 0x0A, 0x0D)


def render_text(data: bytes) -> str:
    """Printable bytes as-is, any stray non-printable as [HH]."""
    out = []
    for b in data:
        if b == 0x09:
            out.append("\\t")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append(f"[{b:02X}]")
    return "".join(out)


def decode_temp(four: bytes) -> str:
    """4 big-endian bytes -> 'raw  int  (C)', flagging implausible / failed-read values."""
    raw = int.from_bytes(four, "big", signed=False)
    val = int.from_bytes(four, "big", signed=True)
    celsius = val / 100.0
    note = ""
    if raw == 0x00000000:
        note = "   <-- 0x00000000: read returned 0 (calibration unset / failed read)"
    elif not (-5000 <= val <= 12000):          # outside ~ -50..120 C
        note = "   <-- implausible: almost certainly a failed I2C read"
    return f">TEMP  raw=0x{raw:08X}  int={val}  ({celsius:.2f} C){note}"


def parse(buf: bytearray, decode: bool):
    """
    Consume complete records from buf, yielding ('text', str) / ('temp', str).
    Returns the unconsumed tail (a partial line or <4 binary bytes) to carry over.
    """
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]

        if is_text_byte(b):
            nl = buf.find(b"\n", i)
            if nl != -1:
                yield ("text", render_text(bytes(buf[i:nl]).rstrip(b"\r")))
                i = nl + 1
                continue
            # No newline yet. If a binary byte follows, the text run is complete;
            # otherwise it's a partial line -> stop and carry it over.
            k = i
            while k < n and is_text_byte(buf[k]):
                k += 1
            if k == n:
                break
            yield ("text", render_text(bytes(buf[i:k])))
            i = k
            continue

        # Non-text byte -> start of a 4-byte temperature group.
        if n - i >= 4:
            group = bytes(buf[i:i + 4])
            yield ("temp", decode_temp(group) if decode else render_text(group))
            i += 4
        else:
            break   # wait for the rest of the group

    del buf[:i]
    return


def main():
    ap = argparse.ArgumentParser(description="stm32-flashfs UART listener")
    ap.add_argument("port", nargs="?", help="serial port (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--timestamp", "-t", action="store_true", help="prefix each line with ms since start")
    ap.add_argument("--no-decode", dest="decode", action="store_false",
                    help="show temps as raw [HH] bytes instead of decoding them")
    ap.add_argument("--raw", action="store_true", help="dump bytes as they arrive, no parsing")
    ap.add_argument("--list", action="store_true", help="list available ports and exit")
    args = ap.parse_args()

    if args.list:
        ports = list(list_ports.comports())
        if not ports:
            print("No serial ports found.")
        for p in ports:
            print(f"{p.device:30} {p.description}")
        return

    port = args.port or find_port()
    if not port:
        sys.exit("No serial port found. Plug in the adapter or pass one explicitly "
                 "(see: python3 tools/listen.py --list)")

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}")

    print(f"# listening on {port} @ {args.baud} baud  (Ctrl-C to quit)", file=sys.stderr)
    start = time.monotonic()
    buf = bytearray()

    def stamp() -> str:
        return f"[{(time.monotonic() - start) * 1000:8.1f} ms] " if args.timestamp else ""

    try:
        while True:
            chunk = ser.read(256)
            if not chunk:
                continue

            if args.raw:
                sys.stdout.write(render_text(chunk))
                sys.stdout.flush()
                continue

            buf.extend(chunk)
            for kind, text in parse(buf, args.decode):
                prefix = stamp() + ("  " if kind == "temp" else "")
                print(f"{prefix}{text}")
            sys.stdout.flush()
    except KeyboardInterrupt:
        if buf:
            # A trailing partial line often pinpoints where the firmware stalled.
            print(f"{stamp()}{render_text(bytes(buf))}  <-- partial (no newline; likely where it stalled)")
        print("\n# stopped", file=sys.stderr)
    finally:
        ser.close()


if __name__ == "__main__":
    main()

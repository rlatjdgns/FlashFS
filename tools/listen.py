#!/usr/bin/env python3
"""
Serial listener for the stm32-flashfs board.

The firmware mixes two kinds of output on the same UART:
  * printable text lines ending in newline ("STM32 booted", "READBACK:", "END",
    and fault diagnostics like "SPI:BYTE-TIMEOUT")
  * 8-byte binary records: big-endian uint32 seq + int32 temp (temp in 0.01 C)

Records between "READBACK:" and "END" are flash readback; all others are live.
Framing and decoding live in tools/stm_protocol.py (shared with visualize.py):
a printable leading byte starts a text line; a non-printable one starts a record.

Baud is 115200 (BRR=69 @ 8 MHz HSI).

Usage:
    python3 tools/listen.py                       # auto-detect port, decode stream
    python3 tools/listen.py /dev/cu.usbserial-0001
    python3 tools/listen.py -t                     # timestamp every line
    python3 tools/listen.py --raw                  # dump every byte as it arrives
    python3 tools/listen.py --list                 # list ports and exit
"""

import argparse
import sys
import time
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stm_protocol import feed

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


def main():
    ap = argparse.ArgumentParser(description="stm32-flashfs UART listener")
    ap.add_argument("port", nargs="?", help="serial port (auto-detect if omitted)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--timestamp", "-t", action="store_true", help="prefix each line with ms since start")
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
    state = {"in_readback": False}

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
            for ev in feed(buf, state):
                if ev[0] == "text":
                    print(f"{stamp()}{ev[1]}")
                else:
                    label = "LIVE" if ev[0] == "live" else "RB  "
                    print(f"{stamp()}  {label}  #{ev[1]}  {ev[2]:.2f} C")
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

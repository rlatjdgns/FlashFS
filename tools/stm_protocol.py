"""Decoder for the STM32 flash-FS UART stream.

The firmware mixes printable text lines (terminated by '\n') with 8-byte binary
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

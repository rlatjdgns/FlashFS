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

"""Shared USB-serial transport for talking to a Master board.

Speaks the line-based JSON protocol described in ../docs/serial-protocol.md.
Used by both master_gui.py and joystick_master_gui.py so the two tools don't
duplicate the same background-thread-plus-queue plumbing.
"""

import json
import threading

import serial

BAUD_RATE = 115200


class SerialLink:
    """Owns the serial port and a background line-reader thread.

    Reads happen off the caller's main thread (blocking readline() would
    freeze a GUI); received lines are handed to `on_line`, which must be
    thread-safe — callers typically marshal them onto the main thread via a
    queue.Queue.
    """

    def __init__(self, on_line, on_error):
        self._ser = None
        self._reader_thread = None
        self._stop_event = threading.Event()
        self._on_line = on_line
        self._on_error = on_error

    @property
    def is_open(self):
        return self._ser is not None and self._ser.is_open

    def connect(self, port):
        self._ser = serial.Serial(port, BAUD_RATE, timeout=0.2)
        self._stop_event.clear()
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()

    def disconnect(self):
        self._stop_event.set()
        if self._reader_thread is not None:
            self._reader_thread.join(timeout=1.0)
        self._reader_thread = None
        if self._ser is not None:
            self._ser.close()
        self._ser = None

    def send(self, obj):
        if not self.is_open:
            raise RuntimeError("not connected")
        line = json.dumps(obj) + "\n"
        self._ser.write(line.encode("utf-8"))
        return line

    def _read_loop(self):
        while not self._stop_event.is_set():
            try:
                raw = self._ser.readline()
            except (serial.SerialException, OSError) as exc:
                if not self._stop_event.is_set():
                    self._on_error(str(exc))
                return
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip()
            if text:
                self._on_line(text)

#!/usr/bin/env python3
"""Display ESP-IDF wind-meter serial JSON at http://localhost:8086."""
import argparse
import json
import math
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = None
        self.received_at = None
        self.message = "Waiting for ESP32 serial data"

    def accept(self, line):
        try:
            payload = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            return  # ESP-IDF boot logs are mixed with the JSON records.
        if not isinstance(payload, dict):
            return
        with self.lock:
            if payload.get("type") == "status" and isinstance(payload.get("message"), str):
                self.message = payload["message"]
            elif payload.get("type") == "wind":
                for key in ("wind_speed_kmh", "wind_direction_deg", "force_ns_n", "force_ew_n"):
                    if key not in payload:
                        return
                    value = payload.get(key)
                    if value is not None and (type(value) not in (int, float) or not math.isfinite(value)):
                        return
                states = (payload.get("ns_state"), payload.get("ew_state"))
                partial = any(payload[key] is None for key in
                              ("wind_speed_kmh", "wind_direction_deg", "force_ns_n", "force_ew_n"))
                if partial and any(value not in ("ready", "zeroing", "disconnected") for value in states):
                    return
                if payload["force_ns_n"] is None or payload["force_ew_n"] is None:
                    if payload["wind_speed_kmh"] is not None or payload["wind_direction_deg"] is not None:
                        return
                try:
                    json.dumps(payload, allow_nan=False)
                except (ValueError, TypeError):
                    return
                self.latest = payload
                self.received_at = time.monotonic()
                if states == ("ready", "ready") or not partial:
                    self.message = "Receiving sensor data"
                else:
                    labels = {"ready": "connected", "zeroing": "zeroing; keep unloaded",
                              "disconnected": "no measurement ready (check configured GPIOs if persistent)"}
                    self.message = f"NS: {labels[states[0]]}; EW: {labels[states[1]]}. Wind calculations need both sensors."

    def snapshot(self):
        with self.lock:
            age = None if self.received_at is None else time.monotonic() - self.received_at
            return {"data": self.latest, "age_s": age,
                    "connected": age is not None and age < 3, "message": self.message}


def read_serial(state, port, baud, stop):
    import serial
    while not stop.is_set():
        try:
            # Avoid deliberately toggling DTR/RTS when opening the device.
            device = serial.Serial(port=None, baudrate=baud, timeout=1)
            device.port = port
            device.dtr = False
            device.rts = False
            device.open()
            with device:
                while not stop.is_set():
                    state.accept(device.read_until(b"\n", size=4096))
        except (serial.SerialException, OSError) as error:
            with state.lock:
                state.message = f"Serial connection error: {error} (retrying)"
            stop.wait(2)


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/":
                body = Path(__file__).with_name("viewer.html").read_bytes()
                content_type = "text/html; charset=utf-8"
            elif self.path == "/api/data":
                body = json.dumps(state.snapshot(), allow_nan=False).encode()
                content_type = "application/json"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serial device, e.g. /dev/cu.usbmodem1234561 or COM3")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--http-port", type=int, default=8086)
    args = parser.parse_args()
    try:
        import serial  # noqa: F401
    except ImportError:
        parser.error("Install dependencies first: python -m pip install -r tools/requirements.txt")
    state = State()
    try:
        server = ThreadingHTTPServer(("127.0.0.1", args.http_port), make_handler(state))
    except OSError as error:
        parser.error(f"Cannot start localhost server: {error}")
    stop = threading.Event()
    reader = threading.Thread(target=read_serial, args=(state, args.port, args.baud, stop), daemon=True)
    reader.start()
    print(f"Open http://localhost:{args.http_port} — reading {args.port} at {args.baud} baud", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
        reader.join(timeout=3)


if __name__ == "__main__":
    main()

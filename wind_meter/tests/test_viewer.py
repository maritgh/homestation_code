"""Exercise the serial protocol and HTTP output without sensor hardware."""
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import urlopen

from tools.viewer import State, make_handler

READING = {"type": "wind", "wind_speed_kmh": 12.5, "wind_direction_deg": 359.0,
           "force_ns_n": 0.1, "force_ew_n": -0.01}


class ViewerTests(unittest.TestCase):
    def test_either_sensor_can_report_independently(self):
        for axis, missing in (("ns", "ew"), ("ew", "ns")):
            with self.subTest(axis=axis):
                state = State()
                reading = dict(READING, wind_speed_kmh=None, wind_direction_deg=None,
                               wind_cardinal=None, median_speed_60s_kmh=None,
                               mean_direction_60s_deg=None, gust_5min_kmh=None)
                reading.update({f"{axis}_state": "ready", f"{missing}_state": "disconnected",
                                f"force_{missing}_n": None, f"raw_{axis}": 123456,
                                f"raw_{missing}": None, f"offset_{axis}": 123000,
                                f"offset_{missing}": None})
                state.accept(json.dumps(reading))
                snapshot = state.snapshot()
                self.assertTrue(snapshot["connected"])
                self.assertEqual(snapshot["data"], reading)
                self.assertIn("Wind calculations need both", snapshot["message"])
                # Replacing a complete reading with a partial one clears wind values.
                state.accept(json.dumps(READING))
                state.accept(json.dumps(reading))
                self.assertIsNone(state.snapshot()["data"]["wind_speed_kmh"])
                state.accept(json.dumps(dict(reading, wind_speed_kmh=12.5)))
                self.assertEqual(state.snapshot()["data"], reading)
                state.received_at -= 4
                self.assertFalse(state.snapshot()["connected"])

    def test_raw_counts_during_zeroing_and_no_sensors(self):
        state = State()
        reading = {"type": "wind", "ns_state": "zeroing", "ew_state": "disconnected",
                   "raw_ns": 123456, "raw_ew": None, "force_ns_n": None,
                   "force_ew_n": None, "wind_speed_kmh": None, "wind_direction_deg": None}
        state.accept(json.dumps(reading))
        self.assertEqual(state.snapshot()["data"]["raw_ns"], 123456)
        self.assertIn("keep unloaded", state.snapshot()["message"])
        reading.update(ns_state="disconnected", raw_ns=None)
        state.accept(json.dumps(reading))
        self.assertTrue(state.snapshot()["connected"])
        self.assertIsNone(state.snapshot()["data"]["raw_ns"])

    def test_logs_invalid_records_and_status(self):
        state = State()
        for line in ['I (100) boot: Starting', 'not json', '[]', '{"type":"wind"}',
                     '{"type":"wind","wind_speed_kmh":NaN}', '\ufffd']:
            state.accept(line)
        self.assertIsNone(state.snapshot()["data"])
        state.accept(json.dumps({"type": "status", "message": "HX711 not ready"}))
        self.assertEqual(state.snapshot()["message"], "HX711 not ready")
        state.accept(json.dumps(READING))
        self.assertTrue(state.snapshot()["connected"])
        self.assertEqual(state.snapshot()["data"], READING)
        state.accept(json.dumps(dict(READING, extra=float("inf"))))
        self.assertEqual(state.snapshot()["data"], READING)
        state.received_at -= 4
        self.assertFalse(state.snapshot()["connected"])

    def test_http_dashboard_and_json(self):
        state = State()
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        address = f"http://127.0.0.1:{server.server_port}"
        try:
            with urlopen(address + "/api/data") as response:
                self.assertIsNone(json.load(response)["data"])
            state.accept(json.dumps(READING))
            with urlopen(address + "/api/data") as response:
                self.assertEqual(json.load(response)["data"], READING)
                self.assertEqual(response.headers["Cache-Control"], "no-store")
            with urlopen(address) as response:
                self.assertIn(b"Wind meter", response.read())
            with self.assertRaises(HTTPError) as error:
                urlopen(address + "/missing")
            self.assertEqual(error.exception.code, 404)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    unittest.main()

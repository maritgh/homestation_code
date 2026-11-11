"""Controles op de computer: python3 -m unittest discover -s tests -v."""
import base64
from collections import deque
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("ir_presets", Path(__file__).resolve().parents[1] / "tools/ir_presets.py")
ir = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ir)
FRAME = base64.b64encode(bytes.fromhex("010002000300feff")).decode()


class FakeSession:
    timeout = 0.1

    def __init__(self, responses):
        self.responses = deque(responses)
        self.commands = []

    def send(self, command):
        self.commands.append(command)

    def receive(self, action, deadline):
        response = self.responses.popleft()
        if response.get("ok") is not True:
            raise ValueError(response["error"])
        if response.get("action") != action:
            raise AssertionError("Unexpected action")
        return response

    def request(self, command):
        self.send(command)
        return self.receive(command["action"], 0)


def status(**presets):
    return {"action": "status", "ok": True, "presets": presets}


def chunk(**presets):
    return dict(ir.FORMAT, action="export", ok=True, presets=presets)


class PresetTests(unittest.TestCase):
    def test_export_combines_chunks_into_existing_v1_format(self):
        session = FakeSession([status(koelen19auto=2, uit=2, koelen18eco=0),
                               chunk(koelen19auto=FRAME), chunk(uit=FRAME),
                               {"action": "export", "ok": True, "complete": True}])
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "presets.json"
            ir.export_presets(session, destination)
            self.assertEqual(json.loads(destination.read_text()),
                             dict(ir.FORMAT, presets={"koelen19auto": FRAME, "uit": FRAME}))
        self.assertEqual(session.commands, [{"action": "status"}, {"action": "export"}])

    def test_incomplete_export_does_not_overwrite_backup(self):
        session = FakeSession([status(koelen19auto=2),
                               {"action": "export", "ok": True, "complete": True}])
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "presets.json"
            destination.write_text("existing backup")
            with self.assertRaisesRegex(ValueError, "onvolledig"):
                ir.export_presets(session, destination)
            self.assertEqual(destination.read_text(), "existing backup")

    def test_failed_export_does_not_create_backup(self):
        session = FakeSession([status(uit=2), chunk(uit=FRAME),
                               {"action": "export", "ok": False, "error": "ESP_FAIL", "complete": True}])
        with tempfile.TemporaryDirectory() as tmp:
            destination = Path(tmp) / "presets.json"
            with self.assertRaises(ValueError):
                ir.export_presets(session, destination)
            self.assertFalse(destination.exists())

    def test_import_sends_one_preset_at_a_time(self):
        session = FakeSession([status(koelen19auto=0, uit=0),
                               {"action": "import", "ok": True},
                               {"action": "import", "ok": True}])
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "old_http_export.json"
            source.write_text(json.dumps(dict(ir.FORMAT, presets={"koelen19auto": FRAME, "uit": FRAME})))
            ir.import_presets(session, source)
        self.assertEqual(session.commands[1:], [dict(ir.FORMAT, action="import", presets={name: FRAME})
                                               for name in ("koelen19auto", "uit")])

    def test_unknown_or_corrupt_import_does_not_write_any_presets(self):
        for presets in ({"koelen19auto": FRAME, "unknown": FRAME},
                        {"koelen19auto": FRAME, "uit": "invalid!"}):
            with self.subTest(presets=presets), tempfile.TemporaryDirectory() as tmp:
                session = FakeSession([status(koelen19auto=0, uit=0)])
                source = Path(tmp) / "presets.json"
                source.write_text(json.dumps(dict(ir.FORMAT, presets=presets)))
                with self.assertRaises(ValueError):
                    ir.import_presets(session, source)
                self.assertTrue(all(command["action"] == "status" for command in session.commands))

    def test_import_stops_after_device_error(self):
        session = FakeSession([status(koelen19auto=0, uit=0),
                               {"action": "import", "ok": False, "error": "ESP_FAIL"}])
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "presets.json"
            source.write_text(json.dumps(dict(ir.FORMAT, presets={"koelen19auto": FRAME, "uit": FRAME})))
            with self.assertRaises(ValueError):
                ir.import_presets(session, source)
        self.assertEqual(len(session.commands), 2)

    def test_maximum_frame_fits_command_limit(self):
        encoded = base64.b64encode(b"\1\0\1\0" * 512).decode()
        command = dict(ir.FORMAT, action="import", presets={"koelen19auto": encoded})
        ir.validate(command)
        self.assertLessEqual(len(ir.encode_command(command)), 4096)

    def test_broker_port_does_not_imply_tls(self):
        self.assertEqual(ir.broker_address("mqtt://145.24.237.237:8883"), ("145.24.237.237", 8883, False))
        self.assertEqual(ir.broker_address("mqtts://example.org"), ("example.org", 8883, True))
        with self.assertRaises(ValueError):
            ir.broker_address("http://192.168.1.100")


if __name__ == "__main__":
    unittest.main()

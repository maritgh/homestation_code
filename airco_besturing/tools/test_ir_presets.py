import base64
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import ir_presets


class PresetTransferTests(unittest.TestCase):
    def setUp(self):
        self.raw = bytes(range(256)) * 8  # Een volledige opname van 512 symbolen.
        self.document = {
            "version": 1, "resolution_hz": 1000000, "carrier_hz": 38000,
            "presets": {
                "koelen17eco": base64.b64encode(self.raw).decode(),
                "uit": base64.b64encode(b'\x01\x80\x02\x00').decode(),
            },
        }

    def test_export_import_preserves_every_byte_and_limits_request_size(self):
        posted = []

        def fake_open(request, timeout):
            if isinstance(request, str):
                content = self.document if request.endswith('/export') else {
                    'koelen17eco': 0, 'uit': 0, 'verwarmen23strong': 100,
                }
                return io.BytesIO(json.dumps(content).encode())
            payload = json.loads(request.data)
            self.assertLessEqual(len(request.data), 4096)
            self.assertEqual(len(payload['presets']), 1)
            posted.append(payload)
            return io.BytesIO(b'Preset geimporteerd')

        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / 'ir_presets.json'
            with patch('ir_presets.urlopen', side_effect=fake_open), redirect_stdout(io.StringIO()):
                ir_presets.export_presets('http://source', destination)
                ir_presets.import_presets('http://target', destination)
            self.assertEqual(json.loads(destination.read_text()), self.document)
        merged = {name: data for payload in posted for name, data in payload['presets'].items()}
        self.assertEqual(merged, self.document['presets'])
        self.assertEqual(base64.b64decode(merged['koelen17eco']), self.raw)

    def test_invalid_last_preset_causes_no_network_writes(self):
        self.document['presets']['uit'] = 'invalid!'
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'bad.json'
            source.write_text(json.dumps(self.document))
            with patch('ir_presets.urlopen') as network:
                with self.assertRaises(ValueError):
                    ir_presets.import_presets('http://target', source)
                network.assert_not_called()

    def test_unknown_preset_rejected_before_upload(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'presets.json'
            source.write_text(json.dumps(self.document))
            with patch('ir_presets.urlopen', return_value=io.BytesIO(b'{"uit": 0}')) as network:
                with self.assertRaisesRegex(ValueError, 'koelen17eco'):
                    ir_presets.import_presets('http://target', source)
                self.assertEqual(network.call_count, 1)

    def test_bad_symbol_sizes_and_metadata_rejected(self):
        for raw in (b'', b'abc', bytes(2052)):
            with self.subTest(size=len(raw)):
                self.document['presets'] = {'uit': base64.b64encode(raw).decode()}
                with self.assertRaises(ValueError):
                    ir_presets.validate(self.document)
        for key, value in (('version', True), ('version', 2), ('resolution_hz', 2000000), ('carrier_hz', 40000)):
            with self.subTest(key=key, value=value):
                document = dict(self.document, presets={}, **{key: value})
                with self.assertRaises(ValueError):
                    ir_presets.validate(document)


if __name__ == '__main__':
    unittest.main()

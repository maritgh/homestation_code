#!/usr/bin/env python3
"""Download of importeer IR-presets zonder extra Python-pakketten."""
import argparse
import base64
import binascii
import json
from pathlib import Path
import sys
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def validate(document):
    if not isinstance(document, dict):
        raise ValueError("Het bestand moet een JSON-object bevatten")
    for key, expected in (("version", 1), ("resolution_hz", 1000000), ("carrier_hz", 38000)):
        if type(document.get(key)) is not int or document[key] != expected:
            raise ValueError(f"Niet ondersteunde waarde voor {key}")
    presets = document.get("presets")
    if not isinstance(presets, dict):
        raise ValueError("presets moet een object zijn")
    for name, encoded in presets.items():
        if not isinstance(name, str) or not name or not isinstance(encoded, str):
            raise ValueError("Ongeldige preset")
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error) as exc:
            raise ValueError(f"Ongeldige base64 voor {name}") from exc
        if not raw or len(raw) > 2048 or len(raw) % 4:
            raise ValueError(f"Ongeldige opnamelengte voor {name}")
    return document


def request_json(url):
    with urlopen(url, timeout=30) as response:
        return json.load(response)


def export_presets(host, destination):
    document = validate(request_json(host + "/presets/export"))
    destination.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print(f"{len(document['presets'])} opnames opgeslagen in {destination}")


def import_presets(host, source):
    document = validate(json.loads(source.read_text(encoding="utf-8")))
    if not document["presets"]:
        raise ValueError("Dit bestand bevat nog geen opnames")
    supported = request_json(host + "/status")
    unknown = document["presets"].keys() - supported.keys()
    if unknown:
        raise ValueError("Doel-ESP kent deze presets niet: " + ", ".join(sorted(unknown)))
    # Valideer het hele bestand voordat de eerste preset wordt geschreven.
    # Per preset versturen houdt het RAM-gebruik van de ESP klein.
    for name, encoded in document["presets"].items():
        payload = dict(document, presets={name: encoded})
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        if len(body) > 4096:
            raise ValueError(f"Preset {name} overschrijdt de uploadlimiet")
        request = Request(host + "/presets/import", data=body,
                          headers={"Content-Type": "application/json"}, method="POST")
        with urlopen(request, timeout=30) as response:
            response.read()
        print(f"Geimporteerd: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("export", "import"))
    parser.add_argument("esp", help="IP-adres of URL van de ESP")
    parser.add_argument("file", type=Path, help="Bijvoorbeeld ir_presets.json")
    args = parser.parse_args()
    host = args.esp.rstrip("/")
    if "://" not in host:
        host = "http://" + host
    try:
        if args.action == "export":
            export_presets(host, args.file)
        else:
            import_presets(host, args.file)
    except HTTPError as exc:
        print(f"HTTP {exc.code}: {exc.read().decode('utf-8', errors='replace')}", file=sys.stderr)
        return 1
    except (OSError, ValueError, URLError) as exc:
        print(f"Mislukt: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Exporteer, importeer of leer IR-presets via de MQTT-broker van de homestation."""
import argparse
import base64
import binascii
import json
import os
from pathlib import Path
import queue
import re
import sys
import tempfile
import threading
import time
from urllib.parse import urlsplit

FORMAT = {"version": 1, "resolution_hz": 1000000, "carrier_hz": 38000}
MAX_JSON_BYTES = 4096
MAX_FRAME_SYMBOLS = 512
SYMBOL_BYTES = 4
MAX_FRAME_BYTES = MAX_FRAME_SYMBOLS * SYMBOL_BYTES


def validate(document):
    if not isinstance(document, dict):
        raise ValueError("Het bestand moet een JSON-object bevatten")
    for key, expected in FORMAT.items():
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
        if not raw or len(raw) > MAX_FRAME_BYTES or len(raw) % SYMBOL_BYTES:
            raise ValueError(f"Ongeldige opnamelengte voor {name}")
    return document


def configured_station():
    config = Path(__file__).resolve().parents[1] / "main" / "mqtt_config.h"
    try:
        match = re.search(r'^\s*#define\s+HOME_STATION_ID\s+"([^"\r\n]+)"',
                          config.read_text(encoding="utf-8"), re.MULTILINE)
        return match[1] if match else None
    except OSError:
        return None


def broker_address(address):
    if "://" not in address:
        address = "mqtt://" + address
    parsed = urlsplit(address)
    if (parsed.scheme not in ("mqtt", "mqtts") or not parsed.hostname or
            parsed.username is not None or parsed.password is not None or
            parsed.path not in ("", "/") or parsed.query or parsed.fragment):
        raise ValueError("Gebruik de MQTT-broker, bijvoorbeeld mqtt://145.24.237.237:8883; niet het ESP-IP of een HTTP-URL")
    tls = parsed.scheme == "mqtts"
    return parsed.hostname, parsed.port or (8883 if tls else 1883), tls


def encode_command(document):
    body = json.dumps(document, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
    if len(body) > MAX_JSON_BYTES:
        raise ValueError("Opdracht overschrijdt de limiet van 4096 bytes")
    return body


class MqttSession:
    """Subscribe before sending commands; process replies in the calling thread."""
    def __init__(self, broker, station, timeout=30, username=None, password=None, cafile=None):
        try:
            import paho.mqtt.client as mqtt
        except ImportError as exc:
            raise ValueError("Installeer eerst de MQTT-bibliotheek: python3 -m pip install -r tools/requirements.txt") from exc
        host, port, tls = broker_address(broker)
        if not station or any(c in station for c in "/+#\0"):
            raise ValueError("Geef een geldige station-id op met --station")
        self.topic = f"homestation/{station}/actuator"
        self.status_topic = self.topic + "/status"
        self.timeout = timeout
        self.messages = queue.Queue()
        self.ready = threading.Event()
        self.error = None
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                  protocol=mqtt.MQTTv311, reconnect_on_failure=False)
        self.client.connect_timeout = timeout
        if username:
            self.client.username_pw_set(username, password)
        if tls:
            self.client.tls_set(ca_certs=cafile)
        elif cafile:
            raise ValueError("--cafile vereist een mqtts://-broker")
        self.client.on_connect = self._on_connect
        self.client.on_subscribe = self._on_subscribe
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        self.host, self.port = host, port

    def _fail(self, message):
        self.error = message
        self.ready.set()
        self.messages.put(None)

    def _on_connect(self, client, userdata, flags, reason_code, properties):
        if reason_code.is_failure:
            self._fail(f"MQTT-verbinding geweigerd: {reason_code}")
            return
        result, _ = client.subscribe(self.status_topic, qos=0)
        if result != 0:
            self._fail("Kan niet abonneren op " + self.status_topic)

    def _on_subscribe(self, client, userdata, mid, reason_codes, properties):
        if not reason_codes or any(code.is_failure for code in reason_codes):
            self._fail("Broker weigert abonnement op " + self.status_topic)
        else:
            self.ready.set()

    def _on_message(self, client, userdata, message):
        if message.topic != self.status_topic or message.retain:
            return
        try:
            body = json.loads(message.payload)
        except (ValueError, UnicodeError):
            return
        if isinstance(body, dict):
            self.messages.put(body)

    def _on_disconnect(self, client, userdata, flags, reason_code, properties):
        self._fail(f"MQTT-verbinding verbroken: {reason_code}")

    def __enter__(self):
        try:
            self.client.connect(self.host, self.port, keepalive=60)
            self.client.loop_start()
            if not self.ready.wait(self.timeout):
                raise TimeoutError("Geen bevestiging van het MQTT-abonnement")
            if self.error:
                raise ValueError(self.error)
        except BaseException:
            self.close()
            raise
        return self

    def close(self):
        self.client.disconnect()
        self.client.loop_stop()

    def __exit__(self, *args):
        self.close()

    def send(self, command):
        if self.error:
            raise ValueError(self.error)
        # Clear replies left over from a previous command; never publish retained actions.
        while True:
            try:
                self.messages.get_nowait()
            except queue.Empty:
                break
        result = self.client.publish(self.topic, encode_command(command), qos=0, retain=False)
        if result.rc != 0:
            raise ValueError("Kan de MQTT-opdracht niet versturen")
        result.wait_for_publish(timeout=self.timeout)
        if not result.is_published():
            raise TimeoutError("MQTT-opdracht kon niet worden verstuurd")

    def receive(self, action, deadline):
        while True:
            if self.error:
                raise ValueError(self.error)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Geen volledig antwoord op '{action}'; controleer of de ESP online is met de MQTT-firmware")
            try:
                response = self.messages.get(timeout=remaining)
            except queue.Empty as exc:
                raise TimeoutError(f"Geen antwoord op '{action}' van {self.status_topic}") from exc
            if response is None:
                continue
            if response.get("action") != action:
                continue
            if response.get("ok") is not True:
                raise ValueError(f"ESP weigert {action}: {response.get('error', 'onbekende fout')}")
            return response

    def request(self, command):
        self.send(command)
        return self.receive(command["action"], time.monotonic() + self.timeout)


def preset_status(session):
    response = session.request({"action": "status"})
    presets = response.get("presets")
    if not isinstance(presets, dict) or any(
            not isinstance(name, str) or type(count) is not int or not 0 <= count <= MAX_FRAME_SYMBOLS
            for name, count in presets.items()):
        raise ValueError("ESP stuurde een ongeldige presetstatus")
    return response


def export_presets(session, destination):
    counts = preset_status(session)["presets"]
    expected = {name: count for name, count in counts.items() if count > 0}
    document = dict(FORMAT, presets={})
    session.send({"action": "export"})
    deadline = time.monotonic() + session.timeout
    while True:
        response = session.receive("export", deadline)
        if response.get("complete") is True:
            break
        chunk = validate(response)
        if len(chunk["presets"]) != 1:
            raise ValueError("ESP moet één preset per exportbericht sturen")
        name, encoded = next(iter(chunk["presets"].items()))
        if name in document["presets"]:
            raise ValueError("Dubbele preset in export: " + name)
        document["presets"][name] = encoded
    actual = {name: len(base64.b64decode(encoded)) // SYMBOL_BYTES
              for name, encoded in document["presets"].items()}
    if actual != expected:
        raise ValueError("Export is onvolledig of opnames zijn ondertussen gewijzigd; bestaand bestand blijft behouden. Probeer opnieuw.")
    # Replace only after the entire export has arrived and passed validation.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=destination.parent,
                                         prefix=destination.name + ".", suffix=".tmp", delete=False) as output:
            temporary = Path(output.name)
            output.write(json.dumps(document, indent=2) + "\n")
        temporary.replace(destination)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"{len(document['presets'])} opnames opgeslagen in {destination}")


def import_presets(session, source):
    document = validate(json.loads(source.read_text(encoding="utf-8")))
    if not document["presets"]:
        raise ValueError("Dit bestand bevat nog geen opnames")
    # Validate every command before changing the first preset on the device.
    commands = [dict(FORMAT, action="import", presets={name: encoded})
                for name, encoded in document["presets"].items()]
    for command in commands:
        encode_command(command)
    supported = preset_status(session)["presets"]
    unknown = document["presets"].keys() - supported.keys()
    if unknown:
        raise ValueError("Doel-ESP kent deze presets niet: " + ", ".join(sorted(unknown)))
    for command in commands:
        session.request(command)
        print("Geïmporteerd: " + next(iter(command["presets"])))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("export", "import", "learn", "send", "status"))
    parser.add_argument("broker", help="MQTT-broker, bijvoorbeeld mqtt://145.24.237.237:8883")
    parser.add_argument("target", nargs="?", help="JSON-bestand voor export/import, preset-id voor learn/send")
    parser.add_argument("--station", default=configured_station(), help="Station-id (standaard HOME_STATION_ID uit main/mqtt_config.h)")
    parser.add_argument("--timeout", type=float, default=30, help="Timeout in seconden (standaard 30)")
    parser.add_argument("--username", default=os.environ.get("MQTT_USERNAME"))
    parser.add_argument("--cafile", help="CA-certificaat voor mqtts://")
    args = parser.parse_args(argv)
    if args.action != "status" and not args.target:
        parser.error("Deze actie vereist een bestand of preset-id")
    if args.action == "status" and args.target:
        parser.error("status heeft geen bestand of preset-id nodig")
    if not 0 < args.timeout < float("inf"):
        parser.error("--timeout moet een positief, eindig getal zijn")
    try:
        with MqttSession(args.broker, args.station, args.timeout, args.username,
                         os.environ.get("MQTT_PASSWORD"), args.cafile) as session:
            if args.action == "export":
                export_presets(session, Path(args.target))
            elif args.action == "import":
                import_presets(session, Path(args.target))
            elif args.action == "status":
                print(json.dumps(preset_status(session), indent=2))
            else:
                response = session.request({"action": args.action, "id": args.target})
                if response.get("id") != args.target:
                    raise ValueError("ESP bevestigde een andere preset")
                if args.action == "learn":
                    print(f"Druk nu op de afstandsbediening voor '{args.target}'. Controleer daarna met status of de opname is opgeslagen.")
                else:
                    print("IR-opdracht verzonden: " + args.target)
    except (OSError, ValueError, RuntimeError) as exc:
        print(f"Mislukt: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())

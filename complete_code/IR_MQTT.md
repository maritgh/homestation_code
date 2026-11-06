# IR via MQTT

De firmware abonneert zich na elke MQTT-verbinding op
`homestation/<station-id>/actuator`. De station-id komt uit
`HOME_STATION_ID` in `main/mqtt_config.h`.

Verstuur bijvoorbeeld op `homestation/marit/actuator`:

```json
{"ir":"koelen19auto"}
```

Dit verstuurt de opgeslagen IR-opname voor die preset, met een draaggolf van
38 kHz. Alle preset-id's staan in `main/ac_presets.h`, inclusief `uit`.
Een preset moet al opgenomen of geïmporteerd zijn; de firmware genereert
geen airco-protocol uit de naam.

De IR-ontvanger zit op GPIO 9 en de zender op GPIO 10. Deze pinnen zijn
instelbaar via `idf.py menuconfig` → `Homestation`. GPIO 8 blijft de klokpin
van de oost/west-windsensor. Bij een pinconflict start de IR-module niet en
wordt de fout gelogd.

## Leren, status en backups

Het hulpscript gebruikt nu de MQTT-broker in plaats van het IP-adres van de
ESP. Installeer de Python-afhankelijkheid eenmalig, bijvoorbeeld in een venv:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r tools/requirements.txt
```

Vanaf de projectmap kun je vervolgens de bestaande JSON-backups gebruiken:

```sh
# Opnames uit de ESP ophalen
python3 tools/ir_presets.py export mqtt://145.24.237.237:8883 ir_presets.json --station marit

# Opnames uit het bestand naar de ESP terugzetten
python3 tools/ir_presets.py import mqtt://145.24.237.237:8883 ir_presets.json --station marit

# Een preset met de fysieke afstandsbediening aanleren
python3 tools/ir_presets.py learn mqtt://145.24.237.237:8883 koelen19auto --station marit

# Opnames controleren of een preset versturen
python3 tools/ir_presets.py status mqtt://145.24.237.237:8883 --station marit
python3 tools/ir_presets.py send mqtt://145.24.237.237:8883 koelen19auto --station marit
```

Zonder `--station` leest het script `HOME_STATION_ID` uit
`main/mqtt_config.h`. De URL hierboven komt overeen met de huidige
brokerconfiguratie. `mqtt://` gebruikt een gewone MQTT-verbinding, ook op
poort 8883; gebruik `mqtts://` wanneer de broker TLS vereist. Optionele
brokercredentials komen uit `MQTT_USERNAME` en `MQTT_PASSWORD`.

Het script wacht eerst op de bevestiging van het MQTT-abonnement en stuurt
daarna de opdracht. Bij export worden de presetberichten samengevoegd tot
hetzelfde JSON-v1-bestand als voorheen. Het bestand wordt pas vervangen na
een volledige export die overeenkomt met de presetstatus. Bij import worden
alle opnames eerst gecontroleerd en daarna één voor één verstuurd, telkens
met bevestiging van de ESP. Onderbreking kan betekenen dat een deel al is
geïmporteerd; het script toont welke presets bevestigd zijn.

Ook deze opdrachten gaan naar dezelfde actuator-topic:

| Functie | JSON |
| --- | --- |
| Verzenden | `{"ir":"koelen19auto"}` |
| Leren | `{"action":"learn","id":"koelen19auto"}` |
| Status | `{"action":"status"}` |
| Exporteren | `{"action":"export"}` |

Na `learn` wordt de volgende ontvangen IR-opname opgeslagen. Het antwoord
`"state":"waiting_for_ir"` bevestigt dat de ontvanger klaarstaat. Druk dan
op de afstandsbediening. Het opslaan wordt gelogd; met `status` lees je het
aantal opgeslagen symbolen per preset en een eventueel actieve leeropdracht.

Antwoorden verschijnen op `homestation/<station-id>/actuator/status`:

```json
{"action":"send","id":"koelen19auto","ok":true}
```

Bij een fout is `ok` gelijk aan `false` en bevat `error` de ESP-IDF-foutnaam.
`ESP_ERR_NOT_FOUND` betekent een onbekende preset of een preset zonder opname.

Een export levert één bericht per opgeslagen preset met `version`,
`resolution_hz`, `carrier_hz` en `presets`. Een afsluitend bericht met
`"action":"export","complete":true` en `ok` meldt het resultaat.
Voor import voeg je `"action":"import"` aan zo'n presetbericht toe of
vervang je de bestaande `action` door `import`:

```json
{
  "action": "import",
  "version": 1,
  "resolution_hz": 1000000,
  "carrier_hz": 38000,
  "presets": {
    "koelen19auto": "<base64 uit een echte export>"
  }
}
```

Importeer één preset per bericht. De base64 bevat maximaal 512 RMT-symbolen
in het bestaande little-endian-formaat. Bij leren worden de niveaus van de
IR-ontvanger geïnverteerd; bij import worden de al voor verzending geschikte
niveaus behouden. Bestaande opnames uit NVS worden ingelezen en opnames in de
partitie `ir_frames` krijgen voorrang. Nieuwe opnames worden daar bewaard.

Berichten mogen maximaal 4096 bytes groot zijn. MQTT-berichten die over
meerdere ontvangst-events verdeeld zijn, worden eerst samengevoegd. Er
kunnen vier opdrachten wachten; een volle wachtrij wordt gelogd.
De subscription en antwoorden gebruiken QoS 0; antwoorden zijn niet retained.

IR-verzending, JSON-verwerking en opslag draaien buiten de MQTT-eventhandler.
Sensoren en scherm blijven hun eigen taken gebruiken. De voormalige HTTP-server
en de extra Wi-Fi-initialisatie uit `airco.c` zijn vervangen door de
bestaande MQTT- en Wi-Fi-module.

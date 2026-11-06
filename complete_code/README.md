# Homestation

Firmware voor een ESP32-S3 die omgevingsmetingen verzamelt, op een OLED toont
en via MQTT publiceert. Met een IR-ontvanger en IR-zender kan het station ook
airco-opdrachten aanleren, bewaren en via MQTT uitvoeren.

Het project gebruikt **ESP-IDF 5.5.3** en **LVGL 9.2.0**. Het is een native
ESP-IDF-project, zonder Arduino. De huidige flashindeling is bedoeld voor
**2 MiB flash**.

## Functies

- Temperatuur en luchtvochtigheid met een SHT45.
- Afstand met een VL53L4CD.
- Watertemperatuur met één DS18B20.
- Windsnelheid en richting met twee HX711-versterkers en een meetplaat.
- SH1106 OLED van 128 × 64 pixels met afstand, temperatuur, vochtigheid,
  watertemperatuur en windsnelheid. Ontbrekende metingen verschijnen als `--`.
- MQTT-publicatie van geldige metingen, standaard elke vijf seconden.
- IR-presets aanleren, verzenden, importeren en exporteren via MQTT.
- Automatisch opnieuw verbinden met Wi-Fi en de MQTT-broker.

## Hardware en aansluitingen

Onderstaande GPIO's komen overeen met de meegeleverde configuratie.

| Onderdeel | Signaal | GPIO / I²C-adres |
| --- | --- | --- |
| Gedeelde I²C-bus | SDA | GPIO 3 |
| Gedeelde I²C-bus | SCL | GPIO 4 |
| SH1106 OLED | I²C | `0x3C` |
| SHT45 | I²C | `0x44` |
| VL53L4CD | I²C | `0x29` |
| DS18B20 | Data | GPIO 11 |
| HX711 noord/zuid | DOUT / SCK | GPIO 5 / 6 |
| HX711 oost/west | DOUT / SCK | GPIO 7 / 8 |
| IR-ontvanger | Data | GPIO 9 |
| IR-zender | Data | GPIO 10 |

De vaste I²C- en DS18B20-pinnen staan in `main/board_config.h`. Wind- en
IR-pinnen zijn instelbaar via `idf.py menuconfig` → **Homestation**.
Gebruik afzonderlijke GPIO's voor alle signalen; de IR-module controleert
of zijn pinnen met de andere aansluitingen conflicteren.

De I²C-bus gebruikt 100 kHz. Gebruik een gemeenschappelijke massa en
ESP32-geschikte signaalniveaus. Voor de DS18B20 is een externe pull-up op
de datalijn nodig; deze implementatie adresseert één sensor met `SKIP ROM`.

## Bouwen en instellen

Installeer ESP-IDF 5.5.3 met de ESP32-S3-toolchain. Voer de opdrachten uit
vanuit de projectmap, in een terminal waarin de ESP-IDF-omgeving actief is:

```sh
# Pas dit pad aan naar je ESP-IDF-installatie.
source /pad/naar/esp-idf/export.sh

idf.py menuconfig
idf.py build
```

Het doel `esp32s3` en de partitietabel staan al in `sdkconfig.defaults`.
Een bestaande `sdkconfig` bevat de daadwerkelijk gebruikte instellingen;
defaults overschrijven deze niet. Alleen bij een nieuwe configuratie of
een ander doelbord is `idf.py set-target esp32s3` nodig.

Bij de eerste build beheert de ESP-IDF Component Manager de LVGL-afhankelijkheid
uit `main/idf_component.yml` en `dependencies.lock`. Bewerk eigen code in
`main/`, niet de gegenereerde map `managed_components/`.

### Wi-Fi en MQTT

1. Stel onder **Homestation** in `menuconfig` het Wi-Fi-netwerk en wachtwoord in.
   `main/mqtt_config.h` gebruikt hiervoor `CONFIG_WIFI_SSID` en
   `CONFIG_WIFI_PASSWORD`. De huidige firmware verwacht een WPA2-netwerk.
2. Stel in `main/mqtt_config.h` de broker-URL (`HOME_STATION_MQTT_URI`),
   station-id (`HOME_STATION_ID`), API-sleutel (`HOME_STATION_API_KEY`) en
   publicatieperiode (`HOME_STATION_PUBLISH_INTERVAL_MS`) in.
3. Bouw opnieuw na een configuratiewijziging.

De meegeleverde persoonlijke instellingen zijn behouden. Neem wachtwoorden
en API-sleutels niet over in openbare documentatie of een openbare repository.

`mqtt://` kiest een gewone MQTT-verbinding; `mqtts://` kiest TLS met de
ESP-IDF-certificatenbundel. Een poortnummer zoals `8883` schakelt op zichzelf
geen TLS in. De firmware stuurt de station-API-sleutel mee in de meetpayload;
MQTT-gebruikersnaam en -wachtwoord zijn momenteel niet ingesteld in de firmware.

### Flashen en loguitvoer

Vervang `/dev/cu.usbmodemXXXX` door de seriële poort van je bord:

```sh
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Verlaat de monitor met `Ctrl+]`. Alleen meekijken kan met
`idf.py -p /dev/cu.usbmodemXXXX monitor`.

Bij het opstarten worden het scherm en de sensoren gestart, gevolgd door
wind, IR en het netwerk. De sensortaak scant de I²C-bus en logt gevonden
adressen. Fouten bij het starten van wind, IR of netwerk worden gelogd;
de al gestarte modules blijven actief.

## MQTT-topics

Alle topics beginnen met `homestation/<station-id>/`. Geldige meetwaarden
worden standaard elke vijf seconden met QoS 0 en zonder retain gepubliceerd.
Niet-beschikbare of ongeldige metingen worden overgeslagen.

| Suffix | Betekenis | Eenheid |
| --- | --- | --- |
| `temperature` | Omgevingstemperatuur | °C |
| `humidity` | Relatieve luchtvochtigheid | % |
| `custom/distance` | Afstand | mm |
| `custom/watertemperatuur` | Watertemperatuur | °C |
| `windSpeed` | Windsnelheid | m/s |
| `custom/windrichting` | Windrichting | graden |
| `actuator` | Binnenkomende IR-opdracht | JSON |
| `actuator/status` | Antwoord op een IR-opdracht | JSON |

Voorbeeld van een meetpayload:

```json
{"apiKey":"<jouw-api-sleutel>","value":21.50}
```

De windrichting gebruikt 0° = noord, 90° = oost, 180° = zuid en 270° = west.
De exacte topicnamen hierboven zijn ook de namen die de firmware verstuurt.

## IR-bediening

Verstuur bijvoorbeeld naar `homestation/<station-id>/actuator`:

```json
{"ir":"koelen19auto"}
```

De preset moet vooraf zijn aangeleerd of geïmporteerd. De beschikbare
preset-id's en hun blijvende NVS-sleutels staan in `main/ac_presets.h`.
Een naam beschrijft een opgeslagen opname; de firmware bouwt daar geen
airco-protocol uit op. De draaggolf is 38 kHz.

Het Python-hulpscript ondersteunt status, leren, verzenden en JSON-backups:

```sh
python3 -m venv .venv
source .venv/bin/activate
python3 -m pip install -r tools/requirements.txt

python3 tools/ir_presets.py status mqtt://<broker>:<poort> --station <station-id>
python3 tools/ir_presets.py learn mqtt://<broker>:<poort> koelen19auto --station <station-id>
python3 tools/ir_presets.py send mqtt://<broker>:<poort> koelen19auto --station <station-id>
python3 tools/ir_presets.py export mqtt://<broker>:<poort> ir_presets.json --station <station-id>
python3 tools/ir_presets.py import mqtt://<broker>:<poort> ir_presets.json --station <station-id>
```

Vervang de waarden tussen `<...>` voor je de opdrachten uitvoert. Na `learn`
druk je op de fysieke afstandsbediening. Controleer daarna met `status` of
de opname is opgeslagen. Zonder `--station` leest het script de station-id
uit `main/mqtt_config.h`.

Zie [IR_MQTT.md](IR_MQTT.md) voor het volledige berichtenformaat,
antwoorden, foutcodes, TLS-opties en het import-/exportproces.

## Windkalibratie

Houd de meetplaat bij het opstarten onbelast: iedere HX711-as gebruikt de
eerste tien succesvolle metingen voor de nulstelling. De taak leest elke
200 ms en levert pas geldige winddata wanneer beide assen zijn genuld en
een meting beschikbaar hebben. Data ouder dan één seconde is ongeldig.

Stel onder **Homestation** de kalibratie per as in (ruwe counts per gram),
de plaatdiameter en de luchtdichtheid in. De meegeleverde instellingen
gebruiken een plaatdiameter van 80 mm en een luchtdichtheid van 1,225 kg/m³.
De firmware berekent de snelheid uit de resulterende kracht en het
plaatoppervlak. Kalibratie en asoriëntatie moeten bij de fysieke opstelling
passen; de meegeleverde waarden zijn geen universele sensorkalibratie.

## Projectstructuur

| Bestand / map | Verantwoordelijkheid |
| --- | --- |
| `main/main.c` | Gedeelde I²C-bus en opstartvolgorde |
| `main/board_config.h` | Vaste aansluitingen van het bord |
| `main/sensors.c`, `sensors.h` | Sensoruitlezing en laatste meetwaarden |
| `main/wind.c`, `wind.h` | HX711, nulstelling en windberekening |
| `main/display.c`, `display_ui.c` | LVGL, schermupdates en meetlabels |
| `main/sh1106.c`, `sh1106.h` | SH1106-schermdriver |
| `main/vl53l4cd.c`, `vl53l4cd.h` | Aangepaste ST-afstandsdriver |
| `main/i2c_shared.c`, `i2c_shared.h` | Vergrendeling van de gedeelde I²C-bus |
| `main/mqtt.c`, `mqtt_config.h` | Wi-Fi, MQTT, meetpublicatie en IR-berichten |
| `main/airco.c`, `airco.h`, `ac_presets.h` | IR-opnames, verzending, opdrachten en opslag |
| `main/Kconfig.projbuild` | Instellingen in het menu Homestation |
| `partitions.csv` | Flashindeling |
| `tools/ir_presets.py` | IR-beheer vanaf een computer |
| `tests/test_ir_presets.py` | Tests voor het Python-hulpscript |
| `ir_presets.json` | Meegeleverde IR-backup |

Sensoren, scherm, wind, IR en MQTT gebruiken eigen FreeRTOS-taken. De laatste
sensor- en windmetingen staan in wachtrijen van één element. Lezers gebruiken
`xQueuePeek()`, zodat MQTT en het scherm dezelfde meting kunnen lezen.
I²C-transacties worden onderling vergrendeld. IR-opdrachten worden in een
aparte taak verwerkt, buiten de MQTT-eventhandler.

## Flashindeling

| Partitie | Beginadres | Grootte | Gebruik |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24 KiB | Algemene opslag en oudere IR-opnames |
| `phy_init` | `0xF000` | 4 KiB | Radio-initialisatie |
| `factory` | `0x10000` | 1728 KiB | Firmware |
| `ir_frames` | `0x1C0000` | 256 KiB | Nieuwe en geïmporteerde IR-opnames |

De laatste partitie eindigt op `0x200000` (2 MiB). De firmware heeft één
app-partitie; OTA-updates zijn niet ingericht. Opnames uit `ir_frames`
krijgen bij het laden voorrang op oudere opnames uit `nvs`. Wis de flash
alleen als je opgeslagen presets mag verliezen; maak eerst een IR-export.

## Controleren en problemen oplossen

```sh
# Tests van de IR-backuptool; geen bord of MQTT-broker nodig.
python3 -m unittest discover -s tests -v

# Firmware compileren en controleren of deze in de app-partitie past.
idf.py build
```

Deze controles bewijzen geen werking van aangesloten hardware of een live
MQTT-verbinding. Controleer die na het flashen met de seriële monitor.

De map `.vscode/` gebruikt relatieve projectpaden. Kies zelf je lokale
ESP-IDF-installatie en seriële poort in de ESP-IDF-extensie. De optionele
devcontainer gebruikt eveneens ESP-IDF 5.5.3. Voor consistente C-opmaak is
een `.clang-format` opgenomen; behoud de oorspronkelijke licentietekst en
registertabel van de aangepaste ST-driver in `main/vl53l4cd.c`.

| Probleem | Controle |
| --- | --- |
| Geen Wi-Fi/MQTT | Controleer Wi-Fi-instellingen, broker-URL, bereikbaarheid en of de broker TLS verwacht. |
| Scherm toont `--` | Bekijk de I²C-scan en sensormeldingen; controleer voeding, massa, adressen en aansluitingen. |
| Geen windpublicatie | Houd de plaat onbelast bij starten en controleer beide HX711-aansluitingen en kalibraties. |
| IR start niet | Controleer GPIO-conflicten en meldingen over de partitie `ir_frames`. |
| IR meldt `ESP_ERR_NOT_FOUND` | Controleer het preset-id en leer of importeer eerst een opname. |
| IR-tool krijgt geen antwoord | Gebruik de broker-URL en de juiste station-id; controleer of de firmware online is. |
| Build verwijst naar een oude projectmap | Gebruik een nieuwe buildmap: `idf.py -B build/nieuw build`. |

De aangepaste VL53L4CD-driver bevat de oorspronkelijke STMicroelectronics-
licentietekst. Behoud die bij verdere wijzigingen en verspreiding.

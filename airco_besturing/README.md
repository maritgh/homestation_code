# Airco besturing via infrarood

ESP-IDF-project voor een ESP32 die infraroodopdrachten van een airco-afstandsbediening
leert, opslaat en opnieuw verstuurt. De ESP32 verbindt met Wi-Fi en biedt een
kleine HTTP-API aan op het lokale netwerk. De actieve firmware is
`main/multiple_ir.c`.

## Hardware

| Onderdeel | Aansluiting |
| --- | --- |
| IR-ontvanger | OUT naar GPIO 8 |
| IR-zender | Aansturing vanaf GPIO 10 |
| ESP32 | Gemeenschappelijke GND met de IR-modules |

Gebruik geschikte IR-modules en signaalniveaus voor je ESP32. Sluit een
IR-led niet rechtstreeks op een GPIO aan; gebruik een passende driver/transistor
en stroombegrenzing. De firmware gebruikt een draaggolf van 38 kHz.

## Instellen, bouwen en flashen

Gebruik een terminal waarin ESP-IDF is geactiveerd. Voer de opdrachten uit
vanuit deze map:

```sh
idf.py set-target esp32s3
idf.py menuconfig
```

Stel onder **Homestation** je eigen Wi-Fi-SSID en wachtwoord in. Stel geen
persoonlijke netwerkgegevens in als gedeelde defaults in broncode in. Bouw en
flash vervolgens:

```sh
idf.py build
idf.py -p PORT flash monitor
```

Vervang `PORT` door de seriële poort van je ESP32. Sluit de monitor met
`Ctrl-]`. Na het verbinden met Wi-Fi meldt de ESP32 zijn IP-adres in de
seriële logs. De partitie-indeling staat in `partitions.csv`; deze is bedoeld
voor 2 MiB flash en reserveert opslag voor IR-presets.

## Presets leren en versturen

Stuur HTTP-verzoeken naar het IP-adres dat in de seriële logs staat. Vervang
`<ESP32-IP>` door dat adres. Om een opdracht aan te leren, stuur je eerst een
POST-verzoek; druk daarna op de bijbehorende knop van de afstandsbediening:

```sh
curl -X POST "http://<ESP32-IP>/learn?id=koelen19auto"
```

Een preset die is aangeleerd, kan zo worden verstuurd:

```sh
curl -X POST "http://<ESP32-IP>/api/ac?id=koelen19auto"
```

De firmware ondersteunt koelen op 18, 19 en 20 °C, verwarmen op 20 t/m 23 °C,
elk met de standen `eco`, `auto` en `strong`, plus `uit`. De volledige IDs en
beschrijvingen staan in `main/ac_presets.h`. Elke preset moet eerst afzonderlijk
worden aangeleerd. Opnames worden in NVS opgeslagen en blijven na een herstart
bewaard.

Beschikbare extra endpoints:

| Methode | Pad | Doel |
| --- | --- | --- |
| `GET` | `/status` | JSON met het aantal opgeslagen IR-symbolen per preset |
| `GET` | `/presets/export` | Exporteer aangeleerde presets als JSON |
| `POST` | `/presets/import` | Importeer één preset per verzoek |

De HTTP-server heeft geen authenticatie. Gebruik deze API alleen op een
vertrouwd lokaal netwerk en stel de ESP32 niet rechtstreeks bloot aan internet.

## Bestanden

- `main/multiple_ir.c`: actieve firmware met Wi-Fi, HTTP-API, IR-ontvangst en
  -verzending, en opslag van presets.
- `main/ac_presets.h`: preset-IDs, beschrijvingen en opslagkeys.
- `partitions.csv`: flashindeling met NVS-opslag voor IR-opnames.
- `main/http_ir.c`: oudere IR-ontvanger/proefimplementatie; niet opgenomen in
  de actieve build.
- `main/copy_paste.c` en `main/copy_write.c`: losse IR-opname- en
  herhaalproeven; niet opgenomen in de actieve build.
- `main/CMakeLists.txt`: selecteert `multiple_ir.c` als firmwarebronbestand.

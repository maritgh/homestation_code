# IR-opnames overzetten als JSON

Configureer eerst de Wi-Fi-gegevens van de ESP32 via `idf.py menuconfig`:
open `Airco besturing`, stel `Wi-Fi SSID` en eventueel `Wi-Fi password` in
en bouw daarna de firmware opnieuw. De instellingen worden lokaal in
`sdkconfig` opgeslagen en staan niet in de broncode.

Flash eerst de nieuwste firmware inclusief partitietabel op beide ESP's:

```sh
idf.py flash
```

Leer de gewenste combinaties eenmalig in op de bron-ESP. Download daarna:

```sh
python3 tools/ir_presets.py export 192.168.1.100 ir_presets.json
```

Vervang het IP-adres door dat van de bron-ESP. Je kunt ook in de browser
`http://192.168.1.100/presets/export` openen; dit downloadt `ir_presets.json`.
Alle beschikbare opnames worden meegenomen, inclusief bestaande opnames uit
de oorspronkelijke NVS-partitie die nog bij de huidige presets horen.
Presets die nog niet zijn ingeleerd ontbreken in het bestand.

Importeer het bestand op de andere ESP:

```sh
python3 tools/ir_presets.py import 192.168.1.101 ir_presets.json
```

Import overschrijft uitsluitend de presets die in het bestand staan en bewaart
ze in flash. Andere opnames blijven behouden. Dezelfde IR-zenderconfiguratie en
een airco die deze signalen accepteert zijn nodig op het doelapparaat.

Het script gebruikt alleen de Python-standaardbibliotheek. Het controleert het
bestand en de ondersteunde presetnamen voordat het de eerste opname uploadt.
Elke opname gaat in een aparte aanvraag om RAM op de ESP te sparen. Als de
verbinding tussentijds wegvalt, zijn eerdere opnames al opgeslagen; voer het
importcommando opnieuw uit om de import af te maken.

## Bestandsformaat (versie 1)

Het JSON-object bevat `version: 1`, `resolution_hz: 1000000`,
`carrier_hz: 38000` en een object `presets`. Elk veld in `presets` gebruikt de
preset-id als naam en een base64-string als waarde. De string bevat de opgenomen
RMT-symbolen als 32-bits woorden in little-endian volgorde: duur0 in bit 0–14,
niveau0 in bit 15, duur1 in bit 16–30 en niveau1 in bit 31. Duren zijn microseconden.
De niveaus zijn al omgekeerd voor verzending: import keert ze niet nogmaals om.
Elke opname bevat 1 t/m 512 symbolen.

`GET /presets/export` levert het volledige bestand.
`POST /presets/import` accepteert hetzelfde formaat met precies één preset
(maximaal 4096 bytes). Gebruik het script om een volledig bestand te importeren.

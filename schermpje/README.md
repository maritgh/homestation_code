# Schermpje en sensoren

ESP-IDF-firmware voor een ESP32-S3 met SH1106 OLED, afstands-, klimaat- en
watertemperatuursensoren. Het scherm toont de actuele metingen; geldige waarden
worden periodiek via MQTT gepubliceerd.

## Aansluitingen

| Onderdeel | Aansluiting | I²C-adres |
| --- | --- | --- |
| SH1106 OLED (128 × 64) | SDA GPIO 3, SCL GPIO 4 | `0x3C` |
| VL53L4CD afstandssensor | SDA GPIO 3, SCL GPIO 4 | `0x29` |
| SHT45 temperatuur/vochtigheid | SDA GPIO 3, SCL GPIO 4 | `0x44` |
| DS18B20 watertemperatuursensor | Data GPIO 11 | 1-Wire |

De I²C-apparaten delen een bus op 100 kHz. Gebruik een gemeenschappelijke
GND en voed de modules met geschikte spanningen en ESP32-veilige signaalniveaus.
De DS18B20-datalijn heeft een externe pull-up nodig. De code ondersteunt één
DS18B20. Ontbrekende of ongeldige metingen verschijnen op het scherm als `--`.

## Bouwen en flashen

Activeer ESP-IDF in je terminal en voer vanuit deze map uit:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Vervang `PORT` door de seriële poort van je ESP32. Sluit de seriële monitor met
`Ctrl-]`. De LVGL-afhankelijkheid (versie 9.2.0) wordt door ESP-IDF Component
Manager opgehaald.

## Wi-Fi en MQTT

Pas de Wi-Fi- en MQTT-instellingen aan in `main/mqtt_config.h`, waaronder de
SSID, het wachtwoord, broker-URI, station-ID en API-sleutel. Houd deze gegevens
privé en zet geen echte sleutels of wachtwoorden in publieke commits.

De standaard publicatiefrequentie is vijf seconden. De huidige topics zijn:

| Topic (na `homestation/<station-id>/`) | Bron | Eenheid |
| --- | --- | --- |
| `temperature` | SHT45 | °C |
| `humidity` | SHT45 | % |
| `custom/distance` | VL53L4CD | mm |
| `custom/watertemperatuur` | DS18B20 | °C |

De payload bevat een API-sleutel en meetwaarde. MQTT probeert automatisch
opnieuw verbinding te maken. Sensor- en displaytaken blijven doorgaan als de
netwerkverbinding niet beschikbaar is. Een melding dat een waarde in de
MQTT-queue is geplaatst, bevestigt niet dat de broker of server deze heeft
opgeslagen.

## Bestanden

- `main/main.c`: initialiseert de gedeelde I²C-bus, het display, de sensoren en MQTT.
- `main/display.c` en `main/sh1106.c`: LVGL-display en SH1106-aansturing.
- `main/display_ui.c`: schermindeling en verversing van de meetwaarden.
- `main/sensors.c`: sensoruitlezing en publicatie van de laatste metingen aan
  display en MQTT.
- `main/i2c_shared.c` en `main/i2c_shared.h`: synchronisatie van gedeelde
  I²C-toegang.
- `main/vl53l4cd.c` en `main/vl53l4cd.h`: VL53L4CD-driver.
- `main/mqtt.c` en `main/mqtt.h`: Wi-Fi/MQTT-verbinding en metingen publiceren.
- `main/mqtt_config.h`: lokale Wi-Fi- en MQTT-configuratie; bevat gevoelige
  instellingen en moet privé blijven.
- `main/idf_component.yml`: externe ESP-IDF-componenten, waaronder LVGL.

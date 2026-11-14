# Homestation

In deze repository staan de firmwareprojecten en het ontwerpdocument voor het
homestation. Hieronder vind je per map wat erin staat; de README in de
projectmappen bevat de installatie- en gebruiksinstructies.

## Projectmappen

- [`airco_besturing/`](airco_besturing/): ESP-IDF-firmware om
  airco-infraroodopdrachten te leren en te versturen. Zie
  [`airco_besturing/README.md`](airco_besturing/README.md).
- [`complete_code/`](complete_code/): de complete versie van het homestation,
  met sensoren, display, windmeting, MQTT en infraroodbediening. Zie
  [`complete_code/README.md`](complete_code/README.md) voor de uitleg.
- [`schermpje/`](schermpje/): ESP32-firmware voor het display en de aangesloten
  sensoren, inclusief MQTT. Zie [`schermpje/README.md`](schermpje/README.md).
- [`wind_meter/`](wind_meter/): apart ESP32-project voor de windmeting, met een
  viewer op de computer. Zie [`wind_meter/README.md`](wind_meter/README.md).

## Ontwerp

[`ontwerpdocument homestation.pdf`](ontwerpdocument%20homestation.pdf) bevat het
ontwerpdocument van het homestation.

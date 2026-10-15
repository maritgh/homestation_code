# ESP-IDF wind meter → localhost:8086

Native ESP-IDF firmware reads two HX711 load-cell amplifiers and sends JSON over
USB serial. A Python viewer on your computer displays the readings at
http://localhost:8086. No Wi-Fi credentials, Arduino libraries, external weather
station, or InfluxDB are needed. Port 8086 is used by the viewer, not a database.
The original Arduino sketch is preserved in `archive/arduino_original.ino`.

## 1. Connect the sensors

Defaults for your **ESP32-S3**:

| HX711 signal | ESP32-S3 connection |
| --- | --- |
| North/south DT / DOUT | GPIO 4 |
| North/south SCK / CLK | GPIO 5 |
| East/west DT / DOUT | GPIO 6 |
| East/west SCK / CLK | GPIO 7 |
| Both GND | GND |
| Both VCC | 3.3 V (for modules supporting 3.3 V supply) |

Use HX711 modules with 3.3 V logic; ESP32 GPIOs must not receive 5 V. Connect each
load cell to its HX711 according to the module/load-cell pinout. Wire colors are
not universal. Check that GPIOs 4–7 are exposed and unused on your particular board.
The copied Arduino pins 26/23/19/18 are unsuitable for this ESP32-S3 setup.

Plug the board's **native USB** connector into your computer. The firmware uses
the ESP32-S3 USB Serial/JTAG console. If you use a USB-to-UART connector instead,
select UART console in `idf.py menuconfig` → Component config → ESP System Settings
→ Channel for console output, and read that connector's serial port.

## 2. Configure, build and flash

Use a terminal with ESP-IDF 5.3 or newer activated (or the ESP-IDF terminal in
VS Code). Your current local installation can be activated with:

```sh
. /Users/maritsnijder/.espressif/v5.5.3/esp-idf/export.sh
idf.py set-target esp32s3
idf.py menuconfig
```

Under **Wind meter**, adjust pins, load-cell calibration, circular plate diameter,
and air temperature/pressure/humidity. Defaults preserve the copied sketch's
calibration and environmental values; they are not calibrated for new load cells.
Calibration is signed raw counts per gram. The default plate diameter is 80 mm.

Find your port (macOS: `ls /dev/cu.*`; Windows: a `COM` port), then flash:

```sh
idf.py -p /dev/cu.usbmodem3101 build flash
```

Replace the port if yours differs. Keep the plate unloaded during startup: the
firmware averages ten readings to zero both sensors. If sensors are absent, it
reports their status and continues polling. Each sensor zeros independently from
its first ten successful readings, including when connected later. A single HX711
is enough: connect it to either the NS or EW pins, and its raw counts, zero offset
and calibrated force will appear. During zeroing, raw counts are already visible.
The absent sensor's readings and all wind calculations show `—` (JSON `null`).
With both sensors ready, it samples about five times per second and computes wind.
Keep a newly connected sensor unloaded while it zeros. Reset the board with no
load to zero again. There is no
automatic zeroing during wind measurements, which could erase actual wind force.

If flashing reports `No serial data received`, close any serial viewer, hold
**BOOT**, press and release **RESET**, then release **BOOT** and retry flashing.
Check the port again: the USB device name may change in download mode. With two
USB connectors, use the native USB connector for the default console setup.

## 3. Start the viewer on your computer

From this project folder:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/requirements.txt
python tools/viewer.py --port /dev/cu.usbmodem3101
```

Open **http://localhost:8086**. Leave the viewer running; press Ctrl-C to stop it.
Close `idf.py monitor` or other serial monitors before starting the viewer,
because the viewer needs exclusive access to the port. On Windows, activate the
venv with `.venv\Scripts\activate` and use `--port COM3` (your actual COM port).
If 8086 is already occupied, stop the existing service or use `--http-port 8087`.
The server binds only to your computer's loopback interface.

The viewer works before sensors are connected: it shows waiting/error status
instead of fabricated measurements. It retries a disconnected USB port and hides
readings older than three seconds. If the port name changes after reconnecting,
restart the viewer with the new port. `GET /api/data` provides the latest reading,
its age, and connection status as JSON.

## Measurements and limits

The dashboard shows forces in newtons, instantaneous estimated wind speed in
km/h, direction/cardinal, rolling 60-second speed median, and maximum gust over
five minutes. Gusts preserve the original upper-half median calculation after
excluding the top eighth of the samples in each 2.5-second window. Windows fill
as readings arrive; no complete 60-second/five-minute history exists at startup.
Direction uses a circular mean for the 60-second statistic so 359° and 1° average
around north. Absolute direction depends on sensor orientation and calibration
signs; check whether the reported vector matches your intended wind-from convention.

Air temperature, humidity and pressure are configured constants, not sensor
measurements. Wind speed preserves the original idealized force/plate formula;
actual plate aerodynamics, load-cell calibration and noise affect accuracy.
The Arduino weather-station fetch, Wi-Fi server, OTA and persistent offsets are
not part of this USB viewer implementation.

Firmware API reference: [Espressif GPIO documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html).

Host-side checks: `python -m unittest discover -s tests -v`. These cover JSON/log
parsing, invalid data rejection, stale readings, and the local HTTP endpoints.
Sensor accuracy still requires testing with connected, calibrated hardware.

# airsensor-mqtt

USB-Raumluft-Sensor-Treiber mit MQTT-Anbindung fuer Linux. Liest VOC-Messwerte (Volatile Organic Compounds) von einem USB-Luftqualitaetssensor und publiziert diese an einen MQTT-Broker zur Einbindung in Heimautomatisierungssysteme.

## Unterstuetzte Hardware

Der Sensor wird unter verschiedenen Marken vertrieben:

- **Conrad** Raumluft-Qualitaetssensor
- **REHAU** Raumluft-Sensor
- Weitere Geraete mit Atmel-Chip (USB Vendor `0x03eb`, Product `0x2013`)

Zur Identifikation des Sensors:

```bash
lsusb | grep 03eb:2013
```

Erwartete Ausgabe:

```
Bus 00x Device 00x: ID 03eb:2013 Atmel Corp.
```

## Schnellstart mit Docker

```bash
docker run --rm --privileged --device=/dev/bus/usb -v /sys:/sys:ro \
  -e MQTT_BROKERNAME=192.168.1.10 \
  -e MQTT_PORT=1883 \
  -e MQTT_CLIENTID=airsensor \
  -e MQTT_TOPIC=home/CO2/voc \
  volschin/airsensor
```

Das Docker-Image ist als Multi-Arch-Build fuer folgende Plattformen verfuegbar:

| Architektur | Beispiel-Hardware |
|-------------|-------------------|
| `linux/amd64` | Standard-PCs, Intel NUCs |
| `linux/arm/v7` | Raspberry Pi 2/3 (32-Bit) |
| `linux/arm64` | Raspberry Pi 3/4/5 (64-Bit) |

## Installation

### Variante 1: Docker (empfohlen)

Docker-Image von Docker Hub:

```bash
docker pull volschin/airsensor:latest
```

Oder lokal bauen:

```bash
git clone https://github.com/volschin/airsensor-mqtt.git
cd airsensor-mqtt
docker build -t airsensor-mqtt .
```

### Variante 2: Lokale Kompilierung

#### Voraussetzungen

- GCC-Compiler
- libusb 1.0 Entwicklungsbibliothek
- Paho MQTT C Client Entwicklungsbibliothek (TLS-faehig)
- OpenSSL Entwicklungsbibliothek
- pthread

Unter Debian/Ubuntu:

```bash
sudo apt-get install gcc libusb-1.0-0-dev libpaho-mqtt-dev libssl-dev
```

#### Kompilieren

```bash
gcc -o airsensor airsensor.c -lusb-1.0 -lpaho-mqtt3cs -lpthread -lssl -lcrypto
```

## Konfiguration

Die gesamte MQTT-Konfiguration erfolgt ueber Umgebungsvariablen. Alle Variablen haben Standardwerte und sind optional -- fehlende Variablen werden automatisch mit den Standardwerten belegt.

| Variable | Beschreibung | Standard |
|----------|-------------|---------|
| `MQTT_BROKERNAME` | Hostname oder IP-Adresse des MQTT-Brokers | `127.0.0.1` |
| `MQTT_PORT` | Port des MQTT-Brokers | `1883` |
| `MQTT_CLIENTID` | Client-ID fuer die MQTT-Verbindung | `airsensor` |
| `MQTT_TOPIC` | MQTT-Topic fuer Sensordaten (JSON) | `home/CO2/voc` |
| `MQTT_USERNAME` | Benutzername fuer MQTT-Authentifizierung (optional) | _(keiner)_ |
| `MQTT_PASSWORD` | Passwort fuer MQTT-Authentifizierung (optional) | _(keines)_ |
| `HA_DISCOVERY_PREFIX` | Praefix fuer Home Assistant Auto-Discovery | `homeassistant` |
| `HA_DEVICE_NAME` | Geraetenamen im Home Assistant | `Air Sensor` |
| `POLL_INTERVAL` | Messintervall in Sekunden (10--3600) | `30` |
| `USB_TIMEOUT` | USB-Timeout in Millisekunden (250--10000) | `1000` |
| `MAX_RETRIES` | Maximale USB-Fehler vor Reconnect (1--20) | `3` |
| `MQTT_TLS` | TLS-Verschluesselung aktivieren (`1` oder `true`) | _(deaktiviert)_ |

## Benutzung

### Kommandozeilen-Optionen

```
./airsensor [-d] [-v] [-o] [-h]
```

| Option | Beschreibung |
|--------|-------------|
| `-d` | Debug-Modus: Ausfuehrliche Ausgaben fuer Fehlersuche |
| `-v` | Nur VOC-Wert ausgeben (Werte ausserhalb 450--2000 ppm werden als `0` ausgegeben) |
| `-o` | Einzelmessung: einmal lesen, dann beenden |
| `-h` | Hilfe anzeigen und beenden |

> **Hinweis:** Im Docker-Image ist `-v` als Standard-Option im ENTRYPOINT konfiguriert.

### Beispiele

**Dauerbetrieb mit Debug-Ausgaben:**

```bash
./airsensor -d
```

**Einzelmessung (z.B. fuer Cronjob):**

```bash
./airsensor -v -o
```

**Docker mit Authentifizierung:**

```bash
docker run --rm --privileged --device=/dev/bus/usb -v /sys:/sys:ro \
  -e MQTT_BROKERNAME=mqtt.example.com \
  -e MQTT_PORT=1883 \
  -e MQTT_CLIENTID=wohnzimmer-sensor \
  -e MQTT_TOPIC=wohnung/wohnzimmer/luftqualitaet \
  -e MQTT_USERNAME=mqttuser \
  -e MQTT_PASSWORD=geheim \
  volschin/airsensor
```

**Docker mit TLS-Verschluesselung:**

```bash
docker run --rm --privileged --device=/dev/bus/usb -v /sys:/sys:ro \
  -e MQTT_BROKERNAME=mqtt.example.com \
  -e MQTT_PORT=8883 \
  -e MQTT_TLS=1 \
  -e MQTT_USERNAME=mqttuser \
  -e MQTT_PASSWORD=geheim \
  volschin/airsensor
```

## Docker Compose

```yaml
services:
  airsensor:
    image: volschin/airsensor:latest
    container_name: airsensor
    restart: unless-stopped
    privileged: true
    devices:
      - /dev/bus/usb:/dev/bus/usb
    volumes:
      - /sys:/sys:ro
    environment:
      MQTT_BROKERNAME: "192.168.1.10"
      MQTT_PORT: "1883"
      MQTT_CLIENTID: "airsensor"
      MQTT_TOPIC: "home/CO2/state"
      MQTT_USERNAME: ""
      MQTT_PASSWORD: ""
      HA_DISCOVERY_PREFIX: "homeassistant"
      HA_DEVICE_NAME: "Air Sensor"
      POLL_INTERVAL: "30"
```

> **Hinweis:** Der Container benoetigt `privileged: true` und Zugriff auf `/sys` (read-only), da libusb 1.0 sowohl die USB-Devicenodes als auch sysfs fuer die Geraeteerkennung benoetigt.

## Messwerte

### VOC-Bereich

Der Sensor liefert VOC-Werte im Bereich von **450 bis 2000 ppm** (laut Spezifikation von AppliedSensor). Intern akzeptiert die Software Werte bis 15001 ppm.

| Bereich (ppm) | Luftqualitaet |
|----------------|---------------|
| 450--600 | Ausgezeichnet |
| 600--1000 | Gut |
| 1000--1500 | Maessig |
| 1500--2000 | Schlecht |

### Sensordaten

Der Sensor liefert folgende Messwerte in einem 16-Byte USB-Response (Little-Endian):

| Wert | Bytes | Beschreibung |
|------|-------|-------------|
| VOC | 2--3 | Volatile Organic Compounds in ppm |
| Debug | 4--5 | Interner Debug-Wert |
| PWM | 6--7 | Heizungs-PWM-Wert |
| r_h | 8--9 | Heizungswiderstand (Rohwert ÷ 100 = Ω) |
| r_s | 12--14 | Sensorwiderstand (24-Bit, in Ω) |

### Ausgabeformat

**Standard-Modus** (ohne `-v`):

```
2026-02-28 14:30:15, VOC: 523, RESULT: OK
```

Bei Werten ausserhalb des gueltigen Bereichs:

```
2026-02-28 14:30:15, VOC: 12345, RESULT: Error value out of range
```

**VOC-only-Modus** (`-v`):

```
523
```

Bei Werten ausserhalb des Bereichs wird `0` ausgegeben.

### MQTT-Nachricht

Alle Messwerte werden als JSON-Objekt auf das konfigurierte Topic publiziert:

- **Topic:** konfigurierbar ueber `MQTT_TOPIC`
- **Payload:** JSON, z.B. `{"voc":523,"r_h":382.21,"r_s":8900110,"debug":736,"pwm":10}`
- **QoS:** 1 (mindestens einmal zugestellt)
- **Retained:** Nein

### Verfuegbarkeit (Availability)

Der Sensor publiziert seinen Online-Status auf `{MQTT_TOPIC}/availability`:

- `online` — nach erfolgreicher MQTT-Verbindung
- `offline` — bei sauberem Herunterfahren oder Verbindungsverlust (via MQTT Last Will and Testament)

### Messintervall

Im Dauerbetrieb wird standardmaessig alle **30 Sekunden** ein neuer Messwert gelesen und publiziert. Das Intervall ist ueber `POLL_INTERVAL` konfigurierbar (10--3600 Sekunden).

### Robustheit

Bei USB-Kommunikationsfehlern versucht das Programm automatisch einen erneuten Leseversuch. Nach `MAX_RETRIES` aufeinanderfolgenden Fehlern wird die USB-Verbindung getrennt und neu aufgebaut.

## Integration in Heimautomatisierung

### Home Assistant

#### Auto-Discovery (empfohlen)

Der Sensor unterstuetzt **MQTT Auto-Discovery** fuer Home Assistant. Beim Start werden automatisch Konfigurationsnachrichten auf die Discovery-Topics publiziert, sodass Home Assistant den Sensor ohne manuelle Konfiguration erkennt und einbindet.

Das Discovery-Topic hat das Format:

```
{HA_DISCOVERY_PREFIX}/sensor/{MQTT_CLIENTID}[_suffix]/config
```

**Beispiel** mit Standardwerten:

```
homeassistant/sensor/airsensor/config
```

Beim Start werden Discovery-Nachrichten fuer folgende Entitaeten publiziert:

| Entitaet | Suffix | Beschreibung |
|----------|--------|-------------|
| VOC | _(keiner)_ | Volatile Organic Compounds (ppm) |
| Heating Resistance | `_rh` | Heizungswiderstand (Ω) |
| Sensor Resistance | `_rs` | Sensorwiderstand (Ω) |
| Warmup | `_warmup` | Aufwaermzeit (min, diagnostisch) |
| Warn Threshold 1 | `_warn1` | Warnschwelle 1 (ppm, diagnostisch) |
| Warn Threshold 2 | `_warn2` | Warnschwelle 2 (ppm, diagnostisch) |

Alle Entitaeten enthalten:
- `object_id` fuer vorhersagbare Entity-IDs (z.B. `sensor.airsensor_voc`)
- `origin` Block mit Integrationsname, Version und Support-URL
- `availability_topic` fuer Online/Offline-Erkennung

Die Mess-Entitaeten (VOC, r_h, r_s) enthalten zusaetzlich:
- `state_class: measurement` fuer Langzeitstatistiken
- `suggested_display_precision` (VOC: 0, r_h: 2, r_s: 0)
- `expire_after` (3× Messintervall) zum automatischen Markieren als unverfuegbar
- `icon: mdi:resistor` fuer die Widerstandssensoren (r_h, r_s)

Die diagnostischen Entitaeten (Warmup, Warn-Schwellen) werden nur publiziert, wenn der Sensor die entsprechenden Abfragen (`FLAGGET?`, `KNOBPRE?`) unterstuetzt.

Die VOC-Discovery-Konfiguration sieht beispielsweise so aus:

```json
{
  "name": "Air Sensor VOC",
  "object_id": "airsensor_voc",
  "state_topic": "home/CO2/voc",
  "value_template": "{{ value_json.voc }}",
  "unit_of_measurement": "ppm",
  "device_class": "volatile_organic_compounds_parts",
  "state_class": "measurement",
  "suggested_display_precision": 0,
  "unique_id": "airsensor_voc",
  "availability_topic": "home/CO2/voc/availability",
  "expire_after": 90,
  "device": {
    "identifiers": ["airsensor"],
    "name": "Air Sensor",
    "model": "USB VOC Sensor",
    "manufacturer": "Atmel",
    "serial_number": "ABC123",
    "sw_version": "1.0"
  },
  "origin": {
    "name": "airsensor-mqtt",
    "sw_version": "0.10.0",
    "support_url": "https://github.com/olcond/airsensor-mqtt"
  }
}
```

> **Hinweis:** Die Felder `serial_number` und `sw_version` im Device-Block werden nur dann gesetzt, wenn der Sensor auf die `*IDN?`-Abfrage beim Start antwortet. Bei aelteren Sensoren ohne diese Unterstuetzung entfallen diese Felder.

Der Geraetenamen und das Discovery-Praefix koennen ueber Umgebungsvariablen angepasst werden:

```bash
docker run --rm --privileged --device=/dev/bus/usb -v /sys:/sys:ro \
  -e MQTT_BROKERNAME=192.168.1.10 \
  -e MQTT_TOPIC=home/CO2/voc \
  -e HA_DEVICE_NAME="Wohnzimmer Sensor" \
  volschin/airsensor
```

#### Manuelle Konfiguration

Falls Auto-Discovery deaktiviert ist oder ein anderes Praefix verwendet wird, kann der Sensor manuell in der `configuration.yaml` eingetragen werden:

```yaml
mqtt:
  sensor:
    - name: "Luftqualitaet VOC"
      state_topic: "home/CO2/voc"
      value_template: "{{ value_json.voc }}"
      unit_of_measurement: "ppm"
      device_class: volatile_organic_compounds_parts
      state_class: measurement
      availability_topic: "home/CO2/voc/availability"
      icon: "mdi:air-filter"
```

### openHAB

Item-Definition:

```
Number Luftqualitaet_VOC "Luftqualitaet [%d ppm]" {mqtt="<[broker:home/CO2/voc:state:default]"}
```

### Node-RED

Einen **mqtt in**-Node konfigurieren mit dem Topic `home/CO2/voc` und dem entsprechenden Broker.

## Fehlerbehebung

### Sensor wird nicht erkannt

1. Pruefen, ob der Sensor angeschlossen ist:
   ```bash
   lsusb | grep 03eb:2013
   ```

2. USB-Berechtigungen pruefen -- das Programm benoetigt Zugriff auf das USB-Geraet:
   ```bash
   # Als root ausfuehren oder passende udev-Regel anlegen
   sudo ./airsensor -d
   ```

3. udev-Regel fuer unprivilegierten Zugriff erstellen:
   ```bash
   echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="03eb", ATTR{idProduct}=="2013", MODE="0666"' \
     | sudo tee /etc/udev/rules.d/99-airsensor.rules
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

### MQTT-Verbindung fehlgeschlagen

```
Failed to connect, return code X
```

- `return code 1`: Unzulaessige Protokoll-Version
- `return code 2`: Client-ID abgelehnt
- `return code 3`: Broker nicht erreichbar
- `return code 4`: Benutzername/Passwort falsch
- `return code 5`: Nicht autorisiert

Pruefen:
- Ist der MQTT-Broker unter `MQTT_BROKERNAME:MQTT_PORT` erreichbar?
- Stimmen Benutzername und Passwort?
- Ist die Client-ID eindeutig (kein anderer Client mit derselben ID verbunden)?

### Geraet nicht gefunden nach ~110 Sekunden

```
Error: Device not found
```

Das Programm sucht bis zu 10 Mal im Abstand von ca. 11 Sekunden nach dem USB-Geraet. Falls es nicht gefunden wird:

- USB-Kabel/Anschluss pruefen
- Sensor an anderem USB-Port versuchen
- Im Docker-Container: `--device=/dev/bus/usb` korrekt angegeben?

### Programm stuerzt ohne Fehlermeldung ab (Segmentation Fault)

Alle Umgebungsvariablen haben Standardwerte und sind optional. Falls dennoch ein Absturz auftritt, bitte im Debug-Modus (`-d`) ausfuehren und die Ausgabe pruefen.

## Entwicklung

### Projektstruktur

Die Anwendung besteht aus `airsensor.c` (Hauptprogramm) und `airsensor.h` (gemeinsame Typen, reine Logikfunktionen und Makros). Unit-Tests (193 Assertions) befinden sich in `tests/test_airsensor.c` und koennen ohne Hardware ausgefuehrt werden (`make test`).

### Kompilieren und testen

```bash
# Docker-Build testen
docker build -t airsensor-mqtt .

# Pre-commit-Hooks installieren
pip install pre-commit
pre-commit install

# Hooks manuell ausfuehren
pre-commit run --all-files
```

### CI/CD

- **Docker Hub:** Automatischer Build und Push bei Aenderungen an `Dockerfile` oder `airsensor.c` auf dem `main`-Branch
- **Monatlicher Rebuild:** Am 28. jedes Monats (Sicherheitsupdates der Basis-Images)
- **Versionierte Releases:** Git-Tags (`vX.Y.Z`) erzeugen ein entsprechendes Docker-Image-Tag

## Lizenz

MIT License -- siehe [LICENSE](LICENSE).

## Credits

- **Rodric Yates** -- Originaler airsensor-linux-usb Treiber
- **Ap15e (MiOS)** -- Anpassungen fuer MiCasaVerde
- **Sebastian Sjoholm** -- Weitere Modifikationen
- **Veit Olschinski** -- MQTT-Integration und Docker-Paketierung

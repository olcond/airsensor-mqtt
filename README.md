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
docker run --rm --device=/dev/bus/usb \
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
- libusb (0.1) Entwicklungsbibliothek
- Paho MQTT C Client Entwicklungsbibliothek
- pthread

Unter Debian/Ubuntu:

```bash
sudo apt-get install gcc libusb-dev libpaho-mqtt-dev
```

#### Kompilieren

```bash
gcc -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

## Konfiguration

Die gesamte MQTT-Konfiguration erfolgt ueber Umgebungsvariablen. **Alle Variablen muessen gesetzt sein** -- bei fehlenden Variablen stuerzt das Programm ab.

| Variable | Beschreibung | Beispiel |
|----------|-------------|---------|
| `MQTT_BROKERNAME` | Hostname oder IP-Adresse des MQTT-Brokers | `192.168.1.10` |
| `MQTT_PORT` | Port des MQTT-Brokers | `1883` |
| `MQTT_CLIENTID` | Client-ID fuer die MQTT-Verbindung | `airsensor` |
| `MQTT_TOPIC` | MQTT-Topic, auf das die Messwerte publiziert werden | `home/CO2/voc` |
| `MQTT_USERNAME` | Benutzername fuer MQTT-Authentifizierung (optional, aber muss gesetzt sein) | `mqttuser` |
| `MQTT_PASSWORD` | Passwort fuer MQTT-Authentifizierung (optional, aber muss gesetzt sein) | `mqttpass` |

> **Hinweis:** Auch `MQTT_USERNAME` und `MQTT_PASSWORD` muessen als Umgebungsvariable vorhanden sein. Falls keine Authentifizierung noetig ist, koennen sie leer gesetzt werden (`MQTT_USERNAME=""` `MQTT_PASSWORD=""`).

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
docker run --rm --device=/dev/bus/usb \
  -e MQTT_BROKERNAME=mqtt.example.com \
  -e MQTT_PORT=1883 \
  -e MQTT_CLIENTID=wohnzimmer-sensor \
  -e MQTT_TOPIC=wohnung/wohnzimmer/luftqualitaet \
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
    devices:
      - /dev/bus/usb:/dev/bus/usb
    environment:
      MQTT_BROKERNAME: "192.168.1.10"
      MQTT_PORT: "1883"
      MQTT_CLIENTID: "airsensor"
      MQTT_TOPIC: "home/CO2/voc"
      MQTT_USERNAME: ""
      MQTT_PASSWORD: ""
```

> **Hinweis:** Der Container benoetigt Zugriff auf den USB-Bus (`/dev/bus/usb`). Alternativ kann auch nur das spezifische USB-Geraet durchgereicht werden, z.B. `/dev/bus/usb/001/005`.

## Messwerte

### VOC-Bereich

Der Sensor liefert VOC-Werte im Bereich von **450 bis 2000 ppm** (laut Spezifikation von AppliedSensor). Intern akzeptiert die Software Werte bis 15001 ppm.

| Bereich (ppm) | Luftqualitaet |
|----------------|---------------|
| 450--600 | Ausgezeichnet |
| 600--1000 | Gut |
| 1000--1500 | Maessig |
| 1500--2000 | Schlecht |

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

Der Messwert wird als einfacher ASCII-String auf das konfigurierte Topic publiziert:

- **Topic:** konfigurierbar ueber `MQTT_TOPIC`
- **Payload:** VOC-Wert als Zahl (z.B. `523`)
- **QoS:** 1 (mindestens einmal zugestellt)
- **Retained:** Nein

### Messintervall

Im Dauerbetrieb wird alle **30 Sekunden** ein neuer Messwert gelesen und publiziert.

## Integration in Heimautomatisierung

### Home Assistant

MQTT-Sensor in der `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - name: "Luftqualitaet VOC"
      state_topic: "home/CO2/voc"
      unit_of_measurement: "ppm"
      device_class: volatile_organic_compounds_parts
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

Haeufigste Ursache: Eine oder mehrere Umgebungsvariablen sind nicht gesetzt. Alle sechs Variablen (`MQTT_BROKERNAME`, `MQTT_PORT`, `MQTT_CLIENTID`, `MQTT_TOPIC`, `MQTT_USERNAME`, `MQTT_PASSWORD`) muessen vorhanden sein.

## Entwicklung

### Projektstruktur

Das gesamte Programm besteht aus einer einzigen Quelldatei (`airsensor.c`, ~330 Zeilen). Es gibt keine Unit-Tests; die Verifizierung erfordert einen echten USB-Sensor.

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

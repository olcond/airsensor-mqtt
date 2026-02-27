# CLAUDE.md — AI Assistant Guide for airsensor-mqtt

## Project Overview

**airsensor-mqtt** is a Linux USB device driver and MQTT publisher written in C. It reads VOC (Volatile Organic Compound) air quality measurements from a USB air sensor (Atmel 0x03eb:0x2013 — sold under brands like Conrad and REHAU) and publishes the readings to an MQTT broker for home automation integration.

- **Language**: C (single-file application)
- **License**: MIT
- **Primary deployment**: Docker container (multi-arch: amd64, arm/v7, arm64)
- **Maintainer**: Veit Olschinski (volschin@googlemail.com)

---

## Repository Structure

```
airsensor-mqtt/
├── airsensor.c                        # Entire application (~330 lines)
├── Dockerfile                         # Multi-stage build (builder + scratch runtime)
├── .pre-commit-config.yaml            # Git hooks: gitleaks, cpplint, whitespace
├── .gitignore                         # Standard C build artifacts
├── LICENSE                            # MIT License
├── README.md                          # Minimal description
└── .github/
    ├── renovate.json5                 # Renovate dependency update bot config
    └── workflows/
        ├── docker-image.yml           # Docker Hub CI/CD (multi-arch builds)
        └── dependency-review.yml      # Vulnerability scanning on PRs
```

---

## Building

### Docker (primary method)

```bash
docker build -t airsensor-mqtt .
```

The Dockerfile uses a two-stage build:
1. **builder** — GCC 14.3 with libusb-dev and libpaho-mqtt-dev installed
2. **runtime** — scratch image with only the statically linked binary

Compilation command:
```bash
gcc -static -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

### Local build (requires libusb-dev and libpaho-mqtt-dev)

```bash
gcc -static -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

---

## Running

```bash
./airsensor [-d] [-v] [-o] [-h]
```

| Flag | Description |
|------|-------------|
| `-d` | Enable debug/verbose output |
| `-v` | Print VOC value only (filters values outside 450–2000 ppm) |
| `-o` | One-shot: read once and exit |
| `-h` | Print help |

### Docker run example

```bash
docker run --rm --device=/dev/bus/usb \
  -e MQTT_BROKERNAME=192.168.1.10 \
  -e MQTT_PORT=1883 \
  -e MQTT_TOPIC=home/CO2/voc \
  volschin/airsensor-mqtt
```

---

## Environment Variables

All MQTT configuration is done via environment variables at runtime:

| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_BROKERNAME` | `127.0.0.1` | MQTT broker hostname or IP |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `MQTT_CLIENTID` | `airsensor` | MQTT client identifier |
| `MQTT_TOPIC` | `home/CO2/voc` | Topic to publish readings |
| `MQTT_USERNAME` | _(empty)_ | Optional authentication username |
| `MQTT_PASSWORD` | _(empty)_ | Optional authentication password |

---

## How the Application Works

### Startup sequence

1. Parse command-line flags
2. Initialize libusb (`usb_init()`)
3. Poll for device (vendor `0x03eb`, product `0x2013`) — retries for up to 100 seconds
4. Claim USB interface and register `SIGTERM` handler for clean shutdown
5. Connect to MQTT broker

### Main read loop (repeats every 30 seconds)

1. Send command bytes `\x40\x68\x2a\x54\x52\x0a\x40...` to the device
2. Read 16-byte response from interrupt endpoint `0x81`
3. Extract VOC value: little-endian 16-bit int at offset `+2`
4. Validate range: 450–15001 ppm (spec range is 450–2000 ppm)
5. Publish validated value to MQTT topic as a string

### Shutdown (SIGTERM)

The `release_usb_device()` signal handler cleanly:
- Releases the USB interface
- Closes the USB device handle
- Disconnects and destroys the MQTT client

---

## Code Conventions

### Style

- **Variables**: snake_case (`print_voc_only`, `one_read`, `iresult`)
- **Macros**: UPPERCASE (`QOS`, `TIMEOUT`)
- **No dynamic allocation**: only static/stack buffers (`char buf[1000]`)
- Logging via `printout()` with format: `YYYY-MM-DD HH:MM:SS, [label] [value]`

### Error handling

- USB operations return codes are checked; failures trigger retry or exit
- MQTT connection failure exits with `EXIT_FAILURE`
- Device not found after 100 seconds causes exit
- Data range validation (450–15001) filters bad reads silently

### USB API note

The code uses the older **libusb 0.1** API (`usb_*` functions), not the modern libusb 1.0 (`libusb_*`) API. Keep this in mind when editing USB-related code.

---

## Pre-commit Hooks

Configured in `.pre-commit-config.yaml`. Install with:

```bash
pip install pre-commit
pre-commit install
```

Hooks that run on every commit:

| Hook | Version | Purpose |
|------|---------|---------|
| `gitleaks` | v8.30.0 | Detect hardcoded secrets |
| `cpplint` | v1.3.5 | C/C++ style linting |
| `end-of-file-fixer` | v6.0.0 | Ensure files end with newline |
| `trailing-whitespace` | v6.0.0 | Remove trailing whitespace |

Run all hooks manually:

```bash
pre-commit run --all-files
```

---

## CI/CD

### Docker image workflow (`.github/workflows/docker-image.yml`)

- **Triggers**: push to `main` (when `Dockerfile` or `airsensor.c` changes), any git tag, scheduled monthly on the 28th
- **Builds for**: `linux/amd64`, `linux/arm/v7`, `linux/arm64`
- **Publishes to**: Docker Hub (credentials via `DOCKER_USERNAME` / `DOCKER_PASSWORD` repository secrets)
- **Tags**: `latest` for main branch; version tags for git tags

### Dependency review workflow (`.github/workflows/dependency-review.yml`)

- **Triggers**: pull requests
- **Purpose**: scans dependencies for known CVEs

### Dependency updates (`.github/renovate.json5`)

Renovate bot automatically creates PRs to update:
- Docker base images
- GitHub Actions versions
- Pre-commit hook versions

---

## Key Constants (airsensor.c)

```c
#define QOS       1         // MQTT QoS level
#define TIMEOUT   10000L    // MQTT operation timeout (ms)

// USB device identifiers
vendor  = 0x03eb  // Atmel
product = 0x2013  // Air sensor

// Sensor value range
min_valid = 450
max_valid = 15001  // spec says 2000, but 15001 is the hard cap

// Read interval
30 seconds between readings

// Device search timeout
10 retries × 10 seconds = 100 seconds max wait
```

---

## Making Changes

1. Edit `airsensor.c` — it is the only source file
2. Test locally with a connected USB device or by examining the MQTT output
3. Rebuild Docker image to verify static compilation succeeds
4. Ensure pre-commit hooks pass before committing
5. Tag a release to trigger a versioned Docker Hub push

There are no unit tests or integration tests in this repository. Verification is done by running the binary against a real USB device.

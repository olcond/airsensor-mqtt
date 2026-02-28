# CLAUDE.md — AI Assistant Guide for airsensor-mqtt

## Project Overview

**airsensor-mqtt** is a Linux USB device driver and MQTT publisher written in C. It reads VOC (Volatile Organic Compound) air quality measurements from a USB air sensor (Atmel 0x03eb:0x2013 — sold under brands like Conrad and REHAU) and publishes the readings to an MQTT broker for home automation integration.

- **Language**: C (single-file application, ~340 lines)
- **License**: MIT
- **Primary deployment**: Docker container (multi-arch: amd64, arm/v7, arm64)
- **Maintainer**: Veit Olschinski (volschin@googlemail.com)
- **Docker Hub image**: `volschin/airsensor` (not `airsensor-mqtt`)
- **Original authors**: Rodric Yates, Ap15e (MiOS), Sebastian Sjoholm

---

## Repository Structure

```
airsensor-mqtt/
├── airsensor.c                        # Entire application (~340 lines)
├── Makefile                           # Build and test targets
├── Dockerfile                         # Multi-stage build (builder + scratch runtime)
├── .pre-commit-config.yaml            # Git hooks: gitleaks, cpplint, whitespace
├── .gitignore                         # Standard C build artifacts
├── LICENSE                            # MIT License
├── README.md                          # Full usage documentation (German)
├── tests/
│   └── test_airsensor.c               # Unit tests (no hardware required)
└── .github/
    ├── renovate.json5                 # Renovate dependency update bot config
    └── workflows/
        ├── docker-image.yml           # Docker Hub CI/CD (multi-arch builds)
        ├── test.yml                   # Unit test CI (runs on ubuntu-latest)
        └── dependency-review.yml      # Vulnerability scanning on PRs
```

---

## Building

### Docker (primary method)

```bash
docker build -t airsensor-mqtt .
```

The Dockerfile uses a two-stage build:
1. **builder** — `gcc:15.2` (pinned by digest) with `libusb-dev` and `libpaho-mqtt-dev` installed
2. **runtime** — `scratch` image with only the statically linked binary

The default Docker entrypoint runs with the `-v` flag (VOC-only output mode):
```dockerfile
ENTRYPOINT ["/airsensor", "-v"]
```

Compilation command (inside builder stage):
```bash
gcc -static -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

### Local build via Makefile

```bash
make          # builds ./airsensor
make test     # builds and runs unit tests
make clean    # removes built binaries
```

The Makefile compiles with `-Wall -Wextra -std=c11`. The test binary does **not** require libusb or libpaho-mqtt.

### Local build (manual)

Requires `libusb-dev` and `libpaho-mqtt-dev`:

```bash
gcc -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

Note: The Dockerfile uses `-static` for a self-contained binary; omit for local builds.

---

## Running

```bash
./airsensor [-d] [-v] [-o] [-h]
```

| Flag | Description |
|------|-------------|
| `-d` | Enable debug/verbose output |
| `-v` | Print VOC value only (prints `0` for values outside 450–15001 ppm) |
| `-o` | One-shot: read once and exit |
| `-h` | Print help and exit |

### Docker run example

```bash
docker run --rm --device=/dev/bus/usb \
  -e MQTT_BROKERNAME=192.168.1.10 \
  -e MQTT_PORT=1883 \
  -e MQTT_TOPIC=home/CO2/voc \
  volschin/airsensor
```

---

## Environment Variables

All MQTT configuration is done via environment variables. The code uses null-checked `getenv()` with compiled-in defaults (see `airsensor.c:92–99`), so missing variables fall back to the defaults listed below rather than segfaulting.

| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_BROKERNAME` | `127.0.0.1` | MQTT broker hostname or IP |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `MQTT_CLIENTID` | `airsensor` | MQTT client identifier |
| `MQTT_TOPIC` | `home/CO2/voc` | Topic to publish readings |
| `MQTT_USERNAME` | _(none)_ | Optional authentication username |
| `MQTT_PASSWORD` | _(none)_ | Optional authentication password |

`MQTT_USERNAME` and `MQTT_PASSWORD` are passed directly to `conn_opts` via `getenv()` (lines 111–112); they return `NULL` if unset, which the Paho library treats as "no authentication".

---

## How the Application Works

### Actual startup sequence (as coded)

1. Read environment variables and construct the MQTT broker address (`tcp://host:port`) via `snprintf` (`airsensor.c:100–101`)
2. Create and connect the MQTT client — **exits with `EXIT_FAILURE` if connection fails** (`airsensor.c:107–117`)
3. Parse command-line flags (`-d`, `-v`, `-o`, `-h`) (`airsensor.c:135–158`)
4. Initialize libusb (`usb_init()`)
5. Poll for USB device (vendor `0x03eb`, product `0x2013`) — up to 10 retries × 11 seconds ≈ 110 seconds
6. Open device, detach any kernel driver, claim USB interface 0
7. Register `SIGTERM` and `SIGINT` handlers (`release_usb_device()`) for clean shutdown
8. Flush any pending USB data with an initial interrupt read on endpoint `0x81`

> Note: MQTT connects **before** the USB device is found. If the USB device is never found or
> fails to open, the MQTT connection is explicitly disconnected and destroyed before `exit(1)`.

### Main read loop — `while(rc == MQTTCLIENT_SUCCESS)`

1. Send 16-byte command packet to USB interrupt endpoint `0x02`:
   `\x40\x68\x2a\x54\x52\x0a\x40\x40\x40\x40\x40\x40\x40\x40\x40\x40`
2. Read 16-byte response from interrupt endpoint `0x81`
3. If read returns 0 bytes, sleep 1 second and retry the read once
4. Extract VOC value: copy 2 bytes from `buf+2`, convert little-endian to host order via `le16toh()` (from `<endian.h>`)
5. Sleep 1 second, then do a flush read on endpoint `0x81`
6. Validate range: 450–15001 ppm
7. If valid: publish to MQTT topic as ASCII string; wait for delivery confirmation
8. If `one_read == 1`: exit immediately
9. Sleep 30 seconds before next cycle
10. Loop exits if `MQTTClient_waitForCompletion()` returns non-success

### Shutdown

The `release_usb_device()` signal handler at `airsensor.c:66` handles both `SIGTERM` and `SIGINT`. It cleanly:
- Releases USB interface (`usb_release_interface`)
- Closes USB device handle (`usb_close`)
- Disconnects and destroys the MQTT client (`MQTTClient_disconnect`, `MQTTClient_destroy`)
- Exits with the USB release return code

---

## Testing

### Unit tests (`tests/test_airsensor.c`)

The test file replicates the self-contained logic from `airsensor.c` without requiring USB hardware or an MQTT broker. It uses a minimal custom test runner (no external framework).

**Test suites:**

| Suite | What it tests |
|-------|--------------|
| VOC range validation | Boundary values (449/450, 15001/15002), typical values, edge cases |
| VOC buffer parsing | Little-endian extraction of bytes 2–3 from the 16-byte USB response |
| MQTT address assembly | `tcp://host:port` string construction |
| `svoc` buffer size | Documents and verifies the known buffer-size issue for 5-digit values |

**Run tests:**
```bash
make test
# or manually:
gcc -Wall -Wextra -o tests/test_airsensor tests/test_airsensor.c
./tests/test_airsensor
```

Tests exit with code `0` on full pass, `1` if any test fails.

> **No hardware tests exist.** Verification of actual USB communication and MQTT publishing requires a real sensor device.

---

## Code Conventions

### Style

- **Variables**: snake_case (`print_voc_only`, `one_read`, `iresult`)
- **Macros**: UPPERCASE (`QOS`, `TIMEOUT`)
- **No dynamic allocation**: only static/stack buffers (`char buf[1000]`, `char svoc[6]`)
- Logging via `printout()` (`airsensor.c:52`) with format: `YYYY-MM-DD HH:MM:SS, [label] [value]`
- Raw `printf()` used directly in the main loop for VOC output lines

### Error handling

- USB operations check return codes; failures print an error or trigger retry/exit
- MQTT connection failure exits with `EXIT_FAILURE`
- Device not found after ~110 seconds: disconnects MQTT, then exits with code 1
- USB open failure: disconnects MQTT, then exits with code 1
- Data range validation (450–15001) suppresses out-of-range reads silently (prints `0` in `-v` mode)
- If `ret == 0` from USB read, a single retry is attempted; no further retry logic

### USB API

The code uses the older **libusb 0.1** API (`usb_*` functions from `<usb.h>`), not the modern
libusb 1.0 (`libusb_*`) API. Keep this in mind when editing USB-related code.

Key USB calls used:
- `usb_init()`, `usb_find_busses()`, `usb_find_devices()`, `usb_get_busses()`
- `usb_open()`, `usb_close()`
- `usb_get_driver_np()`, `usb_detach_kernel_driver_np()`
- `usb_claim_interface()`, `usb_release_interface()`
- `usb_interrupt_read()`, `usb_interrupt_write()`

### MQTT library

Uses the **Paho MQTT C client** synchronous API (`MQTTClient`, not `MQTTAsync`):
- `MQTTClient_create()` with `MQTTCLIENT_PERSISTENCE_NONE`
- QoS level 1, `keepAliveInterval = 70` seconds
- `MQTTClient_publishMessage()` + `MQTTClient_waitForCompletion()` with 10-second timeout

---

## Known Code Issues

These are pre-existing issues in the codebase. Do not silently fix them without discussion.

1. **`command[2048]` declared but never used** (`airsensor.c:125`): Dead variable, presumably
   a leftover from an earlier version.

> **Previously documented issues that have since been fixed:**
> - ~~`svoc[5]` buffer too small~~: Fixed — now `svoc[6]` with `snprintf` (`airsensor.c:308–310`)
> - ~~Environment variable null pointer~~: Fixed — all `getenv()` calls now have null-checked fallback defaults (`airsensor.c:92–99`)
> - ~~`MQTT_USERNAME`/`MQTT_PASSWORD` fetched twice~~: Fixed — now set directly on `conn_opts` once (`airsensor.c:111–112`)
> - ~~SIGINT not handled~~: Fixed — both `SIGTERM` and `SIGINT` are registered (`airsensor.c:210–211`)
> - ~~MQTT not closed on USB timeout exit~~: Fixed — MQTT is disconnected and destroyed before all `exit()` calls in the USB setup path

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

### Unit test workflow (`.github/workflows/test.yml`)

- **Triggers**: push to `main` or any PR when `airsensor.c`, `tests/**`, or `Makefile` change
- **Runs**: `make test` on `ubuntu-latest`
- **Uses**: `step-security/harden-runner` + pinned `actions/checkout`

### Docker image workflow (`.github/workflows/docker-image.yml`)

- **Triggers**: push to `main` (only when `Dockerfile` or `airsensor.c` changes), any git tag,
  scheduled monthly on the 28th at midnight UTC
- **Builds for**: `linux/amd64`, `linux/arm/v7`, `linux/arm64`
- **Publishes to**: Docker Hub as `volschin/airsensor` (credentials via `DOCKER_USERNAME` /
  `DOCKER_PASSWORD` repository secrets)
- **Tags**:
  - `volschin/airsensor:latest` for pushes to `main`
  - `volschin/airsensor:{version}` for git tags
- **Security**: All GitHub Actions are pinned to full commit SHAs (not just version tags) for
  supply-chain security. Uses `step-security/harden-runner` with `egress-policy: audit`.

### Dependency review workflow (`.github/workflows/dependency-review.yml`)

- **Triggers**: all pull requests
- **Purpose**: Scans dependency manifests for known CVEs via `actions/dependency-review-action`
- Also uses `step-security/harden-runner` with pinned SHA

### Dependency updates (`.github/renovate.json5`)

Renovate bot config inherits from `local>volschin/renovate-config` and automatically creates
PRs to update:
- Docker base image digests (gcc image)
- GitHub Actions versions (tracked by SHA + version comment)
- Pre-commit hook versions

---

## Key Constants (`airsensor.c`)

```c
#define QOS       1         // MQTT QoS level
#define TIMEOUT   10000L    // MQTT operation timeout (ms)

// USB device identifiers
vendor  = 0x03eb  // Atmel
product = 0x2013  // Air sensor

// USB endpoints
write_ep = 0x00000002  // interrupt OUT endpoint
read_ep  = 0x00000081  // interrupt IN endpoint

// Sensor value range
min_valid = 450
max_valid = 15001  // spec says 2000 ppm, but hard cap is 15001

// Timing
read_interval    = 30 seconds
flush_sleep      = 1 second  (between write and flush read)
retry_sleep      = 1 second  (on ret==0 retry)

// Device search
max_retries      = 10
retry_interval   = ~11 seconds (1s sleep + 10s sleep per loop)
max_wait         = ~110 seconds
```

---

## Making Changes

1. `airsensor.c` is the **only source file** — all logic lives here
2. Unit tests live in `tests/test_airsensor.c` and test pure logic only (no hardware required)
3. Run tests: `make test`
4. Test full compilation with Docker: `docker build -t airsensor-mqtt .`
5. For local builds: ensure `libusb-dev` and `libpaho-mqtt-dev` are installed
6. Ensure pre-commit hooks pass: `pre-commit run --all-files`
7. Commit and push to trigger CI (unit tests fire on `airsensor.c`/`tests/`/`Makefile` changes;
   Docker build fires on `Dockerfile`/`airsensor.c` changes)
8. Tag a release (`git tag vX.Y.Z && git push --tags`) to trigger a versioned Docker Hub push

---

## GitHub Actions Pinned SHAs (current)

For reference when reviewing or updating workflows:

| Action | Version | SHA |
|--------|---------|-----|
| `step-security/harden-runner` | v2.14.0 | `20cf305ff2072d973412fa9b1e3a4f227bda3c76` |
| `actions/checkout` | v6.0.1 | `8e8c483db84b4bee98b60c0593521ed34d9990e8` |
| `docker/setup-qemu-action` | v3.7.0 | `c7c53464625b32c7a7e944ae62b3e17d2b600130` |
| `docker/setup-buildx-action` | v3.12.0 | `8d2750c68a42422c14e847fe6c8ac0403b4cbd6f` |
| `docker/login-action` | v3.6.0 | `5e57cd118135c172c3672efd75eb46360885c0ef` |
| `docker/build-push-action` | v6.18.0 | `263435318d21b8e681c14492fe198d362a7d2c83` |
| `actions/dependency-review-action` | v4.8.2 | `3c4e3dcb1aa7874d2c16be7d79418e9b7efd6261` |

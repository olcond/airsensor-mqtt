# CLAUDE.md — AI Assistant Guide for airsensor-mqtt

## Project Overview

**airsensor-mqtt** is a Linux USB device driver and MQTT publisher written in C. It reads VOC (Volatile Organic Compound) air quality measurements from a USB air sensor (Atmel 0x03eb:0x2013 — sold under brands like Conrad and REHAU) and publishes the readings to an MQTT broker for home automation integration.

- **Language**: C (single-file application, ~330 lines)
- **License**: MIT
- **Primary deployment**: Docker container (multi-arch: amd64, arm/v7, arm64)
- **Maintainer**: Veit Olschinski (volschin@googlemail.com)
- **Docker Hub image**: `volschin/airsensor` (not `airsensor-mqtt`)
- **Original authors**: Rodric Yates, Ap15e (MiOS), Sebastian Sjoholm

---

## Repository Structure

```
airsensor-mqtt/
├── airsensor.c                        # Entire application (~330 lines)
├── Dockerfile                         # Multi-stage build (builder + scratch runtime)
├── .pre-commit-config.yaml            # Git hooks: gitleaks, cpplint, whitespace
├── .gitignore                         # Standard C build artifacts
├── LICENSE                            # MIT License
├── README.md                          # Minimal description (German)
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
1. **builder** — `gcc:14.3` (pinned by digest) with `libusb-dev` and `libpaho-mqtt-dev` installed
2. **runtime** — `scratch` image with only the statically linked binary

The default Docker entrypoint runs with the `-v` flag (VOC-only output mode):
```dockerfile
ENTRYPOINT ["/airsensor", "-v"]
```

Compilation command (inside builder stage):
```bash
gcc -static -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread
```

### Local build (requires libusb-dev and libpaho-mqtt-dev)

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
| `-v` | Print VOC value only (filters values outside 450–2000 ppm) |
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

All MQTT configuration is done via environment variables. **All variables are required at
runtime** — if any are missing, `getenv()` returns `NULL` and the program will segfault during
the `strcat` address assembly. There is no fallback to the initialized defaults in the code
(see `airsensor.c:90–103`).

| Variable | Intended Default | Description |
|----------|-----------------|-------------|
| `MQTT_BROKERNAME` | `127.0.0.1` | MQTT broker hostname or IP |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `MQTT_CLIENTID` | `airsensor` | MQTT client identifier |
| `MQTT_TOPIC` | `home/CO2/voc` | Topic to publish readings |
| `MQTT_USERNAME` | _(empty)_ | Optional authentication username |
| `MQTT_PASSWORD` | _(empty)_ | Optional authentication password |

> **Known bug**: The default values assigned on lines 90–101 are immediately overwritten by
> `getenv()`. If any required env var is unset, the program segfaults. `MQTT_USERNAME` and
> `MQTT_PASSWORD` are also fetched redundantly (lines 102–103 and again on lines 113–114).

---

## How the Application Works

### Actual startup sequence (as coded)

1. Read all environment variables and construct the MQTT broker address (`tcp://host:port`)
2. Create and connect the MQTT client — **exits with `EXIT_FAILURE` if connection fails**
3. Parse command-line flags (`-d`, `-v`, `-o`, `-h`)
4. Initialize libusb (`usb_init()`)
5. Poll for USB device (vendor `0x03eb`, product `0x2013`) — up to 10 retries × 11 seconds ≈ 110 seconds
6. Open device, detach any kernel driver, claim USB interface 0
7. Register `SIGTERM` handler (`release_usb_device()`) for clean shutdown
8. Flush any pending USB data with an initial interrupt read on endpoint `0x81`

> Note: MQTT connects **before** the USB device is found. If the USB device is never found,
> the MQTT connection is never explicitly closed before `exit(1)`.

### Main read loop — `while(rc == MQTTCLIENT_SUCCESS)`

1. Send 16-byte command packet to USB interrupt endpoint `0x02`:
   `\x40\x68\x2a\x54\x52\x0a\x40\x40\x40\x40\x40\x40\x40\x40\x40\x40`
2. Read 16-byte response from interrupt endpoint `0x81`
3. If read returns 0 bytes, sleep 1 second and retry the read once
4. Extract VOC value: copy 2 bytes from `buf+2`, convert little-endian to host order via `__le16_to_cpu()`
5. Sleep 1 second, then do a flush read on endpoint `0x81`
6. Validate range: 450–15001 ppm
7. If valid: publish to MQTT topic as ASCII string; wait for delivery confirmation
8. If `one_read == 1`: exit immediately
9. Sleep 30 seconds before next cycle
10. Loop exits if `MQTTClient_waitForCompletion()` returns non-success

### Shutdown (SIGTERM only)

The `release_usb_device()` signal handler at `airsensor.c:64` cleanly:
- Releases USB interface (`usb_release_interface`)
- Closes USB device handle (`usb_close`)
- Disconnects and destroys the MQTT client (`MQTTClient_disconnect`, `MQTTClient_destroy`)
- Exits with the USB release return code

> **Note**: Only `SIGTERM` is handled. `SIGINT` (Ctrl+C) is **not** caught, so Ctrl+C during
> interactive use will leave USB resources unreleased.

---

## Code Conventions

### Style

- **Variables**: snake_case (`print_voc_only`, `one_read`, `iresult`)
- **Macros**: UPPERCASE (`QOS`, `TIMEOUT`)
- **No dynamic allocation**: only static/stack buffers (`char buf[1000]`, `char svoc[5]`)
- Logging via `printout()` (`airsensor.c:51`) with format: `YYYY-MM-DD HH:MM:SS, [label] [value]`
- Raw `printf()` used directly in the main loop for VOC output lines

### Error handling

- USB operations check return codes; failures print an error or trigger retry/exit
- MQTT connection failure exits with `EXIT_FAILURE`
- Device not found after ~110 seconds exits with code 1
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

1. **`svoc[5]` buffer too small** (`airsensor.c:298`): The buffer for formatting the VOC value as
   a string is only 5 bytes. Values ≥ 10000 (5 digits) require 6 bytes including the null
   terminator. Since `max_valid` is 15001, this is a latent buffer overflow.

2. **Environment variable null pointer** (`airsensor.c:90–101`): Default values are overwritten
   by `getenv()` without null-checking. Any missing required env var causes a segfault.

3. **`command[2048]` declared but never used** (`airsensor.c:127`): Dead variable, presumably
   a leftover from an earlier version.

4. **`MQTT_USERNAME`/`MQTT_PASSWORD` fetched twice** (`airsensor.c:102–103`, `113–114`): `getenv`
   is called redundantly; the `username`/`password` local variables declared at lines 102–103 are
   never actually used in the connection options.

5. **SIGINT not handled**: Only `SIGTERM` is registered, so Ctrl+C exits without cleanup.

6. **MQTT not closed on USB timeout exit** (`airsensor.c:192`): If the USB device is never found,
   `exit(1)` is called without disconnecting the MQTT client.

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
2. There are **no unit tests or integration tests**; verification requires a real USB device
3. Test compilation with Docker: `docker build -t airsensor-mqtt .`
4. For local builds: ensure `libusb-dev` and `libpaho-mqtt-dev` are installed
5. Ensure pre-commit hooks pass: `pre-commit run --all-files`
6. Commit and push to trigger Docker Hub CI (only fires on changes to `Dockerfile` or `airsensor.c`)
7. Tag a release (`git tag vX.Y.Z && git push --tags`) to trigger a versioned Docker Hub push

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

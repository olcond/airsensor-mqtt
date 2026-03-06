# CLAUDE.md — AI Assistant Guide for airsensor-mqtt

## Project Overview

**airsensor-mqtt** is a Linux USB device driver and MQTT publisher written in C. It reads VOC (Volatile Organic Compound) air quality measurements from a USB air sensor (Atmel 0x03eb:0x2013 — sold under brands like Conrad and REHAU) and publishes the readings to an MQTT broker for home automation integration.

- **Language**: C (single-file application, ~930 lines)
- **License**: MIT
- **Primary deployment**: Docker container (multi-arch: amd64, arm/v7, arm64)
- **Maintainer**: Veit Olschinski (volschin@googlemail.com)
- **Docker Hub image**: `volschin/airsensor` (not `airsensor-mqtt`)
- **Original authors**: Rodric Yates, Ap15e (MiOS), Sebastian Sjoholm

---

## Repository Structure

```
airsensor-mqtt/
├── airsensor.c                        # Entire application (~930 lines)
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

All configuration is done via environment variables. The code uses null-checked `getenv()` with compiled-in defaults (`airsensor.c:318–332`), so missing variables fall back to the defaults listed below rather than segfaulting. Integer variables use `parse_env_int()` with bounds clamping.

| Variable | Default | Description |
|----------|---------|-------------|
| `MQTT_BROKERNAME` | `127.0.0.1` | MQTT broker hostname or IP |
| `MQTT_PORT` | `1883` | MQTT broker port |
| `MQTT_CLIENTID` | `airsensor` | MQTT client identifier |
| `MQTT_TOPIC` | `home/CO2/voc` | Topic to publish sensor data (JSON) |
| `MQTT_USERNAME` | _(none)_ | Optional authentication username |
| `MQTT_PASSWORD` | _(none)_ | Optional authentication password |
| `HA_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant MQTT discovery topic prefix |
| `HA_DEVICE_NAME` | `Air Sensor` | Device name shown in Home Assistant |
| `POLL_INTERVAL` | `30` | Sensor poll interval in seconds (10–3600) |
| `USB_TIMEOUT` | `1000` | USB read/write timeout in milliseconds (250–10000) |
| `MAX_RETRIES` | `3` | Max consecutive USB failures before reconnect (1–20) |

`MQTT_USERNAME` and `MQTT_PASSWORD` are passed directly to `conn_opts` via `getenv()` (`airsensor.c:351–352`); they return `NULL` if unset, which the Paho library treats as "no authentication".

`HA_DISCOVERY_PREFIX` and `HA_DEVICE_NAME` control the auto-discovery messages published on startup.

---

## How the Application Works

### Actual startup sequence (as coded)

1. Read environment variables and construct the MQTT broker address (`tcp://host:port`) via `snprintf` (`airsensor.c:333–335`)
2. Configure MQTT Last Will and Testament (LWT) to publish `"offline"` on `{MQTT_TOPIC}/availability` (`airsensor.c:341–345`)
3. Create and connect the MQTT client — **exits with `EXIT_FAILURE` if connection fails** (`airsensor.c:347–358`)
4. Publish `"online"` on the availability topic (retained) (`airsensor.c:361–365`)
5. Parse command-line flags (`-d`, `-v`, `-o`, `-h`) (`airsensor.c:370–393`)
6. Initialize libusb (`usb_init()`)
7. Poll for USB device (vendor `0x03eb`, product `0x2013`) — up to 10 retries × 11 seconds ≈ 110 seconds
8. Open device, detach any kernel driver, claim USB interface 0
9. Register `SIGTERM` and `SIGINT` handlers (`release_usb_device()`) for clean shutdown (`airsensor.c:462–463`)
10. Flush any pending USB data with an initial interrupt read on endpoint `0x81`
11. Query device identification via `query_device_id()` — sends `*IDN?` USB command to retrieve serial number and firmware version, stored in `device_serial` and `device_firmware` globals (`airsensor.c:291–314`)
12. Query device flags via `FLAGGET?` — retrieves warmup time, burn-in period, and other flags (`airsensor.c:533–553`)
13. Query warn thresholds via `KNOBPRE?` — retrieves warn1 and warn2 VOC thresholds (`airsensor.c:558–578`)
14. Publish Home Assistant MQTT auto-discovery configs (retained, QoS 1) for up to six entities — VOC, r_h, r_s (measurement sensors with `state_class`, `availability_topic`, `expire_after`) and warmup, warn1, warn2 (diagnostic sensors with `entity_category`) — to `{HA_DISCOVERY_PREFIX}/sensor/{clientid}[_suffix]/config` (`airsensor.c:580–730`)
15. Publish one-time diagnostic data (warmup, burn_in, warn1, warn2) as retained JSON on `{MQTT_TOPIC}/diag` (`airsensor.c:735–748`)

> Note: MQTT connects **before** the USB device is found. If the USB device is never found or
> fails to open, the MQTT connection is explicitly disconnected and destroyed before `exit(1)`.
> The auto-discovery messages are published **after** USB setup and device queries, so they can
> include device serial number, firmware version, flags, and warn thresholds when available.

### Main read loop — `while(rc == MQTTCLIENT_SUCCESS)`

1. Build sequenced 16-byte poll command via `build_poll_command()` with rotating sequence byte (0x67–0xFF, FHEM protocol)
2. Send command to USB interrupt endpoint `0x02`; on write failure, increment `fail_count` and retry or reconnect if `fail_count >= max_retries`
3. Read 16-byte response from interrupt endpoint `0x81` (up to 3 chunks)
4. If read returns 0 bytes, sleep 1 second and retry the read once
5. Extract sensor data from the response buffer (all little-endian):
   - VOC: 2 bytes from `buf+2` via `le16toh()`
   - Debug: 2 bytes from `buf+4` via `le16toh()`
   - PWM: 2 bytes from `buf+6` via `le16toh()`
   - r_h (heating resistance): 2 bytes from `buf+8` via `le16toh()`, divided by 100.0
   - r_s (sensor resistance): 3 bytes from `buf[12..14]` as 24-bit LE integer
6. Sleep 1 second, then do a flush read on endpoint `0x81`
7. Validate VOC range: 450–15001 ppm
8. If valid: publish JSON payload `{"voc":N,"r_h":N.NN,"r_s":N,"debug":N,"pwm":N}` to MQTT topic; wait for delivery confirmation
9. If `one_read == 1`: exit immediately
10. Sleep `poll_interval` seconds before next cycle (default 30, configurable 10–3600)
11. Loop exits if `MQTTClient_waitForCompletion()` returns non-success
12. On USB write/read failures: after `max_retries` consecutive errors, close and re-open USB device

### Shutdown

The `release_usb_device()` signal handler at `airsensor.c:100` handles both `SIGTERM` and `SIGINT` (registered at `airsensor.c:462–463`). It cleanly:
- Releases USB interface (`usb_release_interface`)
- Closes USB device handle (`usb_close`)
- Publishes `"offline"` on the availability topic (retained)
- Disconnects and destroys the MQTT client (`MQTTClient_disconnect`, `MQTTClient_destroy`)
- Exits with the USB release return code

---

## Testing

### Unit tests (`tests/test_airsensor.c`)

The test file replicates the self-contained logic from `airsensor.c` without requiring USB hardware or an MQTT broker. It uses a minimal custom test runner (no external framework).

**Test suites (164 assertions):**

| Suite | What it tests |
|-------|--------------|
| VOC range validation | Boundary values (449/450, 15001/15002), typical values, edge cases |
| VOC buffer parsing | Little-endian extraction of bytes 2–3 from the 16-byte USB response |
| Debug value parsing | Little-endian extraction of bytes 4–5 |
| PWM value parsing | Little-endian extraction of bytes 6–7 |
| r_h parsing | Little-endian extraction of bytes 8–9 (heating resistance) |
| r_s parsing | 24-bit LE extraction of bytes 12–14 (sensor resistance) |
| MQTT address assembly | `tcp://host:port` string construction |
| HA discovery | Discovery topic format, required JSON fields incl. `state_class`, `availability_topic`, `expire_after` |
| `*IDN?` parsing | Serial number and firmware extraction from device response |
| JSON payload | Consolidated JSON state payload format |
| Poll command | Sequenced poll command building and sequence number wrapping |
| Environment parsing | `parse_env_int()` with defaults, bounds clamping |
| Retry logic | Fail count thresholds for retry vs reconnect decisions |
| Data command | `FLAGGET?`/`KNOBPRE?`/`*IDN?` command building with sequence numbers |
| FLAGGET? parsing | Device flags (warmup, burn_in, etc.) extraction |
| KNOBPRE? parsing | Warn threshold extraction |
| Diagnostic discovery | Diagnostic entity discovery payloads with `entity_category` |
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
- Logging via `printout()` (`airsensor.c:54`) with format: `YYYY-MM-DD HH:MM:SS, [label] [value]`
- Raw `printf()` used directly in the main loop for VOC output lines

### Error handling

- USB operations check return codes; failures increment `fail_count` and trigger retry or reconnect after `max_retries`
- MQTT connection failure exits with `EXIT_FAILURE`
- Device not found after ~110 seconds: disconnects MQTT, then exits with code 1
- USB open failure: disconnects MQTT, then exits with code 1
- Data range validation (450–15001) suppresses out-of-range reads silently (prints `0` in `-v` mode)
- If `ret == 0` from USB read, a single retry is attempted
- After `max_retries` consecutive USB failures: USB device is closed and re-opened automatically
- MQTT LWT publishes `"offline"` on broker-detected disconnect; clean shutdown also publishes `"offline"`

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
- Last Will and Testament (LWT) configured via `MQTTClient_willOptions` for availability signaling

---

## Known Code Issues

There are currently no known code issues.

> **Previously documented issues that have since been fixed:**
> - ~~`command[2048]` declared but never used~~: Fixed — dead variable removed
> - ~~`svoc[5]` buffer too small~~: Fixed — now `svoc[6]` with `snprintf`
> - ~~Environment variable null pointer~~: Fixed — all `getenv()` calls now have null-checked fallback defaults
> - ~~`MQTT_USERNAME`/`MQTT_PASSWORD` fetched twice~~: Fixed — now set directly on `conn_opts` once
> - ~~SIGINT not handled~~: Fixed — both `SIGTERM` and `SIGINT` are registered
> - ~~MQTT not closed on USB timeout exit~~: Fixed — MQTT is disconnected and destroyed before all `exit()` calls in the USB setup path
> - ~~Incorrect sensor data parsing~~: Fixed — humidity was fake (PWM high byte), resistance byte offsets were wrong; corrected per FHEM CO20 reference

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

// Timing (defaults, configurable via env vars)
poll_interval    = 30 seconds  (POLL_INTERVAL, 10–3600)
usb_timeout      = 1000 ms     (USB_TIMEOUT, 250–10000)
max_retries      = 3           (MAX_RETRIES, 1–20)
flush_sleep      = 1 second    (between write and flush read)

// Poll sequence (FHEM protocol)
seq_start        = 0x67
seq_end          = 0xFF        // wraps to seq_start (153-step cycle)

// Device search (startup)
search_retries   = 10
search_interval  = ~11 seconds (1s sleep + 10s sleep per loop)
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

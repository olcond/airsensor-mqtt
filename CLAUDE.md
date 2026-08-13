# airsensor-mqtt repository notes

C daemon that reads the Atmel `03eb:2013` VOC sensor over libusb and publishes
state, diagnostics, availability and Home Assistant discovery through the
synchronous Paho MQTT client.

## Commands

```bash
make test
make
docker build -t airsensor-mqtt .
pre-commit run --all-files
```

`make test` is hardware-free. Changes to USB I/O, reconnect behavior, MQTT or HA
discovery also require a real sensor and broker E2E check; state explicitly when
that validation was not possible.

## Invariants

- Pure shared logic belongs in `airsensor.h`; both `airsensor.c` and
  `tests/test_airsensor.c` include it.
- Keep the code allocation-free and preserve bounded static/stack buffers.
- Sensor fields are little-endian: VOC at bytes 2-3, debug 4-5, PWM 6-7,
  `r_h` 8-9 and 24-bit `r_s` at 12-14.
- Valid VOC values are 450-15001. Out-of-range readings are suppressed (`0` in
  `-v` mode).
- MQTT connects before USB discovery; HA discovery is published only after USB
  setup and device queries so serial, firmware and thresholds are available.
- Preserve retained online/offline availability, LWT, QoS 1, delivery waits and
  cleanup on every USB setup failure and normal shutdown.
- USB failures count toward `MAX_RETRIES`, then close and reopen the device.
- `MQTT_TLS` uses `ssl://` and server-certificate authentication. Never weaken
  TLS or log credentials.
- Docker is multi-arch. Keep runtime libraries aligned with the dynamically
  linked builder output.

Match the existing C style and keep `-Wall -Wextra -std=c11` clean. Do not copy
dependency/action version snapshots into this file; manifests and workflows are
authoritative.

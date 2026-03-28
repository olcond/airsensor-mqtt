FROM gcc:15.2@sha256:9cc747b141fb69baaff237936f742f579fe6439e5b3b533b1c40d82374d220a0 AS builder
ENV DEBIAN_FRONTEND=noninteractive
ENV TERM=xterm

# Install base environment
RUN apt-get update \
  && apt-get install -qqy --no-install-recommends apt-utils ca-certificates \
  apt-transport-https \
  libusb-1.0-0-dev libpaho-mqtt-dev libssl-dev

COPY airsensor.c airsensor.h /
RUN gcc -o airsensor airsensor.c -lusb-1.0 -lpaho-mqtt3cs -lpthread -lssl -lcrypto

FROM debian:trixie-slim
RUN apt-get update \
  && apt-get install -qqy --no-install-recommends \
  libusb-1.0-0 libpaho-mqtt1.3 ca-certificates \
  && rm -rf /var/lib/apt/lists/*
COPY --from=builder /airsensor /airsensor
USER nobody
ENTRYPOINT ["/airsensor", "-v"]

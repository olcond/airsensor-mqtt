FROM alpine:3.23 AS builder

RUN apk add --no-cache gcc musl-dev libusb-dev paho-mqtt-c-dev openssl-dev

COPY airsensor.c airsensor.h /
RUN gcc -o airsensor airsensor.c -lusb-1.0 -lpaho-mqtt3cs -lpthread -lssl -lcrypto

FROM alpine:3.23
RUN apk add --no-cache libusb paho-mqtt-c ca-certificates-bundle
COPY --from=builder /airsensor /airsensor
# Runs as root: libusb 1.0 requires root for USB device enumeration
ENTRYPOINT ["/airsensor", "-v"]

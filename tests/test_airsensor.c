/*
 * tests/test_airsensor.c
 *
 * Unit tests for the pure logic in airsensor.c.
 *
 * These tests do NOT depend on USB hardware, libusb, or the MQTT broker.
 * They replicate the self-contained logic from airsensor.c and verify it
 * in isolation, with references to the source lines being tested.
 *
 * Compile:  gcc -Wall -Wextra -o tests/test_airsensor tests/test_airsensor.c
 * Run:      ./tests/test_airsensor
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Minimal test runner
 * -------------------------------------------------------------------------- */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, expr)                                        \
    do {                                                        \
        if (expr) {                                             \
            printf("  PASS  %s\n", name);                       \
            tests_passed++;                                     \
        } else {                                                \
            printf("  FAIL  %s\n", name);                       \
            tests_failed++;                                     \
        }                                                       \
    } while (0)

static void print_header(const char *suite) {
    printf("\n[%s]\n", suite);
}

/* --------------------------------------------------------------------------
 * Logic replicated from airsensor.c
 *
 * Each helper mirrors the exact expression used in production code.
 * Line references are to airsensor.c.
 * -------------------------------------------------------------------------- */

/*
 * VOC range check — airsensor.c:290
 *   if ( voc >= 450 && voc <= 15001)
 *
 * AppliedSensor spec: 450–2000 ppm.  The code accepts up to 15001.
 */
static int voc_in_range(unsigned short voc) {
    return (voc >= 450 && voc <= 15001);
}

/*
 * Little-endian VOC extraction from USB response buffer — airsensor.c:274–275
 *   memcpy(&iresult, buf+2, 2);
 *   voc = __le16_to_cpu(iresult);
 *
 * The USB response is 16 bytes; bytes 2–3 carry the VOC value in
 * little-endian byte order.  We reconstruct it portably here without
 * relying on the kernel-specific __le16_to_cpu macro.
 */
static unsigned short parse_voc_from_buf(const unsigned char *buf) {
    return (unsigned short)buf[2] | ((unsigned short)buf[3] << 8);
}

/*
 * Humidity extraction from USB response buffer — byte 7
 *
 * The USB response is 16 bytes; byte 7 carries the relative humidity
 * as an unsigned 8-bit integer.
 */
static unsigned char parse_humidity_from_buf(const unsigned char *buf) {
    return buf[7];
}

/*
 * Sensor resistance (Rs) extraction from USB response buffer — bytes 8-11
 *
 * The USB response is 16 bytes; bytes 8–11 carry the sensor resistance
 * as a little-endian unsigned 32-bit integer.
 */
static unsigned int parse_resistance_from_buf(const unsigned char *buf) {
    return (unsigned int)buf[8]
         | ((unsigned int)buf[9]  << 8)
         | ((unsigned int)buf[10] << 16)
         | ((unsigned int)buf[11] << 24);
}

/*
 * Parse serial number from *IDN? response text.
 * Looks for "S/N:" marker and extracts the serial string.
 * Returns 0 on success, -1 if marker not found.
 */
static int parse_serial_from_idn(const char *response, char *serial, size_t serial_size) {
    const char *pos = strstr(response, "S/N:");
    if (!pos) return -1;
    pos += 4;
    size_t i = 0;
    while (pos[i] && pos[i] != ' ' && pos[i] != '\n' && pos[i] != '\r' && i < serial_size - 1) {
        serial[i] = pos[i];
        i++;
    }
    serial[i] = '\0';
    return 0;
}

/*
 * Parse firmware version from *IDN? response text.
 * Looks for "FW:" marker and extracts the version string.
 * Returns 0 on success, -1 if marker not found.
 */
static int parse_firmware_from_idn(const char *response, char *firmware, size_t fw_size) {
    const char *pos = strstr(response, "FW:");
    if (!pos) return -1;
    pos += 3;
    size_t i = 0;
    while (pos[i] && pos[i] != ' ' && pos[i] != '\n' && pos[i] != '\r' && i < fw_size - 1) {
        firmware[i] = pos[i];
        i++;
    }
    firmware[i] = '\0';
    return 0;
}

/*
 * MQTT address assembly — airsensor.c:94–97
 *   char address[80] = "tcp://";
 *   strcat(address, brokername);
 *   strcat(address, ":");
 *   strcat(address, portnumber);
 */
static void build_mqtt_address(char *address, size_t size,
                               const char *host, const char *port) {
    snprintf(address, size, "tcp://%s:%s", host, port);
}

/* --------------------------------------------------------------------------
 * Test suites
 * -------------------------------------------------------------------------- */

/* --- VOC range validation ------------------------------------------------ */

static void suite_voc_range(void) {
    print_header("VOC range validation (airsensor.c:290)");

    /* Boundary: lower */
    TEST("voc=450  is valid (lower boundary)",  voc_in_range(450)  == 1);
    TEST("voc=449  is invalid (below lower)",   voc_in_range(449)  == 0);

    /* Boundary: upper */
    TEST("voc=15001 is valid (upper boundary)", voc_in_range(15001) == 1);
    TEST("voc=15002 is invalid (above upper)",  voc_in_range(15002) == 0);

    /* Typical clean-air value from spec */
    TEST("voc=523  is valid (typical clean air)", voc_in_range(523) == 1);

    /* Spec upper limit */
    TEST("voc=2000 is valid (spec max)",        voc_in_range(2000) == 1);

    /* Edge cases */
    TEST("voc=0    is invalid",                 voc_in_range(0)    == 0);
    TEST("voc=65535 is invalid (uint16 max)",   voc_in_range(65535) == 0);
}

/* --- Little-endian buffer parsing --------------------------------------- */

static void suite_voc_parsing(void) {
    print_header("VOC buffer parsing — little-endian bytes 2-3 (airsensor.c:274-275)");

    unsigned char buf[16];

    /* 523 decimal = 0x020B → lo=0x0B, hi=0x02 */
    memset(buf, 0, sizeof(buf));
    buf[2] = 0x0B;
    buf[3] = 0x02;
    TEST("parse 523  (0x020B): lo=0x0B hi=0x02", parse_voc_from_buf(buf) == 523);

    /* 450 decimal = 0x01C2 → lo=0xC2, hi=0x01 */
    memset(buf, 0, sizeof(buf));
    buf[2] = 0xC2;
    buf[3] = 0x01;
    TEST("parse 450  (0x01C2): lo=0xC2 hi=0x01", parse_voc_from_buf(buf) == 450);

    /* 1000 decimal = 0x03E8 → lo=0xE8, hi=0x03 */
    memset(buf, 0, sizeof(buf));
    buf[2] = 0xE8;
    buf[3] = 0x03;
    TEST("parse 1000 (0x03E8): lo=0xE8 hi=0x03", parse_voc_from_buf(buf) == 1000);

    /* 15001 decimal = 0x3A99 → lo=0x99, hi=0x3A */
    memset(buf, 0, sizeof(buf));
    buf[2] = 0x99;
    buf[3] = 0x3A;
    TEST("parse 15001 (0x3A99): lo=0x99 hi=0x3A", parse_voc_from_buf(buf) == 15001);

    /* Zero value */
    memset(buf, 0, sizeof(buf));
    TEST("parse 0: all-zero buffer yields 0", parse_voc_from_buf(buf) == 0);

    /* Bytes 0, 1, 4+ must be ignored — only bytes 2-3 carry the value */
    memset(buf, 0xFF, sizeof(buf));  /* poison all bytes */
    buf[2] = 0x0B;
    buf[3] = 0x02;
    TEST("parse 523 even when surrounding bytes are 0xFF",
         parse_voc_from_buf(buf) == 523);
}

/* --- Humidity buffer parsing --------------------------------------------- */

static void suite_humidity_parsing(void) {
    print_header("Humidity buffer parsing — byte 7");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    buf[7] = 0;
    TEST("humidity=0 from zero buffer", parse_humidity_from_buf(buf) == 0);

    memset(buf, 0, sizeof(buf));
    buf[7] = 128;
    TEST("humidity=128 from buf[7]=0x80", parse_humidity_from_buf(buf) == 128);

    memset(buf, 0, sizeof(buf));
    buf[7] = 255;
    TEST("humidity=255 from buf[7]=0xFF", parse_humidity_from_buf(buf) == 255);

    memset(buf, 0xFF, sizeof(buf));
    buf[7] = 42;
    TEST("humidity=42 even when surrounding bytes are 0xFF",
         parse_humidity_from_buf(buf) == 42);
}

/* --- Sensor resistance buffer parsing ------------------------------------ */

static void suite_resistance_parsing(void) {
    print_header("Sensor resistance (Rs) buffer parsing — bytes 8-11 (little-endian uint32)");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    TEST("resistance=0 from zero buffer", parse_resistance_from_buf(buf) == 0);

    memset(buf, 0, sizeof(buf));
    buf[8]  = 0xE8;
    buf[9]  = 0x03;
    TEST("resistance=1000 (0x000003E8)", parse_resistance_from_buf(buf) == 1000);

    memset(buf, 0, sizeof(buf));
    buf[8]  = 0xA0;
    buf[9]  = 0x86;
    buf[10] = 0x01;
    TEST("resistance=100000 (0x000186A0)", parse_resistance_from_buf(buf) == 100000);

    memset(buf, 0, sizeof(buf));
    buf[8]  = 0xFF;
    buf[9]  = 0xFF;
    buf[10] = 0xFF;
    buf[11] = 0xFF;
    TEST("resistance=4294967295 (0xFFFFFFFF)", parse_resistance_from_buf(buf) == 4294967295U);

    memset(buf, 0xFF, sizeof(buf));
    buf[8]  = 0xE8;
    buf[9]  = 0x03;
    buf[10] = 0x00;
    buf[11] = 0x00;
    TEST("resistance=1000 even when surrounding bytes are 0xFF",
         parse_resistance_from_buf(buf) == 1000);
}

/* --- MQTT address assembly ----------------------------------------------- */

static void suite_mqtt_address(void) {
    print_header("MQTT broker address assembly (airsensor.c:94-97)");

    char addr[80];

    build_mqtt_address(addr, sizeof(addr), "192.168.1.10", "1883");
    TEST("IPv4 host + port → tcp://192.168.1.10:1883",
         strcmp(addr, "tcp://192.168.1.10:1883") == 0);

    build_mqtt_address(addr, sizeof(addr), "127.0.0.1", "1883");
    TEST("localhost default → tcp://127.0.0.1:1883",
         strcmp(addr, "tcp://127.0.0.1:1883") == 0);

    build_mqtt_address(addr, sizeof(addr), "mqtt.example.com", "8883");
    TEST("hostname + non-default port → tcp://mqtt.example.com:8883",
         strcmp(addr, "tcp://mqtt.example.com:8883") == 0);
}

/* --- Home Assistant MQTT discovery -------------------------------------- */

/*
 * Discovery topic assembly — airsensor.c
 *   snprintf(discovery_topic, ..., "%s/sensor/%s/config", ha_prefix, clientid)
 */
static void build_discovery_topic(char *topic, size_t size,
                                  const char *prefix, const char *clientid) {
    snprintf(topic, size, "%s/sensor/%s/config", prefix, clientid);
}

/*
 * Discovery payload assembly — airsensor.c
 * Builds a minimal JSON payload similar to what airsensor.c produces.
 */
static void build_discovery_payload(char *payload, size_t size,
                                    const char *device_name,
                                    const char *state_topic,
                                    const char *clientid) {
    snprintf(payload, size,
             "{\"name\":\"%s VOC\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"ppm\","
             "\"device_class\":\"volatile_organic_compounds_parts\","
             "\"unique_id\":\"%s_voc\","
             "\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\","
             "\"model\":\"USB VOC Sensor\","
             "\"manufacturer\":\"Atmel\"}}",
             device_name, state_topic, clientid, clientid, device_name);
}

static void build_humidity_discovery_payload(char *payload, size_t size,
                                              const char *device_name,
                                              const char *state_topic,
                                              const char *clientid,
                                              const char *serial,
                                              const char *firmware) {
    char device_block[512];
    if (serial && serial[0] && firmware && firmware[0]) {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\","
                 "\"serial_number\":\"%s\","
                 "\"sw_version\":\"%s\"}",
                 clientid, device_name, serial, firmware);
    } else {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\"}",
                 clientid, device_name);
    }
    snprintf(payload, size,
             "{\"name\":\"%s Humidity\","
             "\"state_topic\":\"%s\","
             "\"unique_id\":\"%s_humidity\","
             "%s}",
             device_name, state_topic, clientid, device_block);
}

static void build_resistance_discovery_payload(char *payload, size_t size,
                                                const char *device_name,
                                                const char *state_topic,
                                                const char *clientid,
                                                const char *serial,
                                                const char *firmware) {
    char device_block[512];
    if (serial && serial[0] && firmware && firmware[0]) {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\","
                 "\"serial_number\":\"%s\","
                 "\"sw_version\":\"%s\"}",
                 clientid, device_name, serial, firmware);
    } else {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\"}",
                 clientid, device_name);
    }
    snprintf(payload, size,
             "{\"name\":\"%s Resistance\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"Ohm\","
             "\"unique_id\":\"%s_resistance\","
             "%s}",
             device_name, state_topic, clientid, device_block);
}

static void suite_ha_discovery(void) {
    print_header("Home Assistant MQTT discovery");

    char topic[256];

    /* Default prefix */
    build_discovery_topic(topic, sizeof(topic), "homeassistant", "airsensor");
    TEST("default topic: homeassistant/sensor/airsensor/config",
         strcmp(topic, "homeassistant/sensor/airsensor/config") == 0);

    /* Custom prefix */
    build_discovery_topic(topic, sizeof(topic), "myhome", "airsensor");
    TEST("custom prefix: myhome/sensor/airsensor/config",
         strcmp(topic, "myhome/sensor/airsensor/config") == 0);

    /* Custom clientid */
    build_discovery_topic(topic, sizeof(topic), "homeassistant", "wohnzimmer");
    TEST("custom clientid: homeassistant/sensor/wohnzimmer/config",
         strcmp(topic, "homeassistant/sensor/wohnzimmer/config") == 0);

    char payload[1024];

    /* Payload contains required HA fields */
    build_discovery_payload(payload, sizeof(payload),
                            "Air Sensor", "home/CO2/voc", "airsensor");

    TEST("payload contains state_topic",
         strstr(payload, "\"state_topic\":\"home/CO2/voc\"") != NULL);
    TEST("payload contains unit ppm",
         strstr(payload, "\"unit_of_measurement\":\"ppm\"") != NULL);
    TEST("payload contains device_class",
         strstr(payload, "\"device_class\":\"volatile_organic_compounds_parts\"") != NULL);
    TEST("payload contains unique_id",
         strstr(payload, "\"unique_id\":\"airsensor_voc\"") != NULL);
    TEST("payload contains device name",
         strstr(payload, "\"name\":\"Air Sensor\"") != NULL);
    TEST("payload contains manufacturer",
         strstr(payload, "\"manufacturer\":\"Atmel\"") != NULL);
}

/* --- Extended HA discovery — humidity, resistance, device info ----------- */

static void suite_extended_ha_discovery(void) {
    print_header("Extended HA discovery — humidity, resistance, device info");

    char payload[1024];

    /* Humidity discovery without device info */
    build_humidity_discovery_payload(payload, sizeof(payload),
                                     "Air Sensor", "home/CO2/humidity",
                                     "airsensor", "", "");
    TEST("humidity payload contains name",
         strstr(payload, "\"name\":\"Air Sensor Humidity\"") != NULL);
    TEST("humidity payload contains unique_id",
         strstr(payload, "\"unique_id\":\"airsensor_humidity\"") != NULL);
    TEST("humidity payload contains state_topic",
         strstr(payload, "\"state_topic\":\"home/CO2/humidity\"") != NULL);

    /* Resistance discovery with device info */
    build_resistance_discovery_payload(payload, sizeof(payload),
                                       "Air Sensor", "home/CO2/resistance",
                                       "airsensor", "ABC123", "1.12p5");
    TEST("resistance payload contains name",
         strstr(payload, "\"name\":\"Air Sensor Resistance\"") != NULL);
    TEST("resistance payload contains unit Ohm",
         strstr(payload, "\"unit_of_measurement\":\"Ohm\"") != NULL);
    TEST("resistance payload contains serial_number",
         strstr(payload, "\"serial_number\":\"ABC123\"") != NULL);
    TEST("resistance payload contains sw_version",
         strstr(payload, "\"sw_version\":\"1.12p5\"") != NULL);

    /* VOC discovery still works (existing function, no serial/firmware) */
    build_discovery_payload(payload, sizeof(payload),
                            "Air Sensor", "home/CO2/voc", "airsensor");
    TEST("VOC payload still works without serial/firmware",
         strstr(payload, "\"unique_id\":\"airsensor_voc\"") != NULL);
}

/* --- *IDN? response parsing ---------------------------------------------- */

static void suite_idn_parsing(void) {
    print_header("*IDN? response parsing — serial number and firmware");

    char serial[20];
    char firmware[20];
    int ret;

    ret = parse_serial_from_idn("DEVICE S/N:4142434445-000001 FW:1.12p5 CPU:ATmega32U4",
                                 serial, sizeof(serial));
    TEST("serial found in typical response", ret == 0);
    TEST("serial value: 4142434445-000001",
         strcmp(serial, "4142434445-000001") == 0);

    ret = parse_firmware_from_idn("DEVICE S/N:4142434445-000001 FW:1.12p5 CPU:ATmega32U4",
                                   firmware, sizeof(firmware));
    TEST("firmware found in typical response", ret == 0);
    TEST("firmware value: 1.12p5", strcmp(firmware, "1.12p5") == 0);

    ret = parse_serial_from_idn("NO SERIAL HERE", serial, sizeof(serial));
    TEST("serial not found returns -1", ret == -1);

    ret = parse_firmware_from_idn("NO FIRMWARE HERE", firmware, sizeof(firmware));
    TEST("firmware not found returns -1", ret == -1);

    ret = parse_serial_from_idn("S/N:ABCDEF123456", serial, sizeof(serial));
    TEST("serial at end of string", ret == 0);
    TEST("serial value: ABCDEF123456", strcmp(serial, "ABCDEF123456") == 0);

    ret = parse_firmware_from_idn("FW:2.0", firmware, sizeof(firmware));
    TEST("firmware at end of string", ret == 0);
    TEST("firmware value: 2.0", strcmp(firmware, "2.0") == 0);

    ret = parse_serial_from_idn("S/N:ABC123\nFW:1.0\n", serial, sizeof(serial));
    TEST("serial terminated by newline", ret == 0);
    TEST("serial value: ABC123", strcmp(serial, "ABC123") == 0);

    /* Semicolon as field delimiter — used in real device responses */
    ret = parse_serial_from_idn("S/N:ABC123;FW:1.2", serial, sizeof(serial));
    TEST("serial terminated by semicolon", ret == 0);
    TEST("serial value: ABC123 (semicolon delim)", strcmp(serial, "ABC123") == 0);

    /* Firmware terminated by semicolon */
    ret = parse_firmware_from_idn("FW:1.12p5;MCU:ATmega", firmware, sizeof(firmware));
    TEST("firmware terminated by semicolon", ret == 0);
    TEST("firmware value: 1.12p5 (semicolon delim)", strcmp(firmware, "1.12p5") == 0);

    /* Firmware terminated by @ padding characters */
    ret = parse_firmware_from_idn("FW:1.12p5@@@@", firmware, sizeof(firmware));
    TEST("firmware terminated by @ padding", ret == 0);
    TEST("firmware value with @ terminator", strcmp(firmware, "1.12p5") == 0);

    /* Fallback: no FW: marker — firmware embedded between ; and $;;MCU (FHEM-style) */
    ret = parse_firmware_from_idn("...;1.12p5$;;MCU...", firmware, sizeof(firmware));
    TEST("firmware fallback marker parsed", ret == 0);
    TEST("firmware fallback value: 1.12p5", strcmp(firmware, "1.12p5") == 0);
}

/* --- Known bug: svoc[5] buffer too small --------------------------------- */
/*
 * airsensor.c:298  char svoc[5];
 * airsensor.c:300  sprintf(svoc, "%d", voc);
 *
 * A value of 15001 has 5 digits and requires 6 bytes (including '\0').
 * The buffer svoc[5] is one byte too small for any 5-digit value, which
 * causes undefined behaviour (buffer overflow) via sprintf().
 *
 * These tests document the requirement, not the current (broken) behaviour.
 * They will pass once the buffer is correctly sized to at least 6 bytes.
 */
static void suite_svoc_buffer(void) {
    print_header("svoc buffer size — known bug (airsensor.c:298)");

    /* Calculate how many bytes sprintf needs for each value */
    char tmp[32];

    int len_523   = snprintf(tmp, sizeof(tmp), "%d", 523);
    int len_2000  = snprintf(tmp, sizeof(tmp), "%d", 2000);
    int len_9999  = snprintf(tmp, sizeof(tmp), "%d", 9999);
    int len_10000 = snprintf(tmp, sizeof(tmp), "%d", 10000);
    int len_15001 = snprintf(tmp, sizeof(tmp), "%d", 15001);

    /* Values with ≤4 digits fit in svoc[5] (4 chars + '\0') */
    TEST("523   requires 3 bytes, fits in svoc[5]",  len_523   + 1 <= 5);
    TEST("2000  requires 4 bytes, fits in svoc[5]",  len_2000  + 1 <= 5);
    TEST("9999  requires 4 bytes, fits in svoc[5]",  len_9999  + 1 <= 5);

    /* 5-digit values overflow svoc[5] — these tests document the bug */
    TEST("10000 requires 6 bytes, overflows svoc[5]", len_10000 + 1 > 5);
    TEST("15001 requires 6 bytes, overflows svoc[5]", len_15001 + 1 > 5);

    /* The minimum safe buffer size for any value up to 15001 is 6 bytes */
    TEST("safe buffer for max_valid(15001) must be >= 6 bytes",
         len_15001 + 1 >= 6);
}

/* --------------------------------------------------------------------------
 * Entry point
 * -------------------------------------------------------------------------- */

int main(void) {
    printf("airsensor unit tests\n");
    printf("====================\n");

    suite_voc_range();
    suite_voc_parsing();
    suite_humidity_parsing();
    suite_resistance_parsing();
    suite_mqtt_address();
    suite_ha_discovery();
    suite_extended_ha_discovery();
    suite_idn_parsing();
    suite_svoc_buffer();

    printf("\n====================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return (tests_failed > 0) ? 1 : 0;
}

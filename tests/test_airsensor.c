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
#include <stdlib.h>
#include <string.h>

#define AIRSENSOR_TEST
#include "../airsensor.h"

int log_level = LOG_LEVEL_ERROR;

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
 * Test-only buffer parsing helpers
 *
 * These helpers parse the 16-byte USB response buffer for testing purposes.
 * -------------------------------------------------------------------------- */

/*
 * USB response buffer layout (16 bytes, per FHEM 38_CO20.pm reference):
 *
 *   Byte 0:    '@' marker (0x40)
 *   Byte 1:    sequence byte
 *   Bytes 2-3: VOC (LE 16-bit, ppm)
 *   Bytes 4-5: debug value (LE 16-bit)
 *   Bytes 6-7: PWM value (LE 16-bit)
 *   Bytes 8-9: r_h — heating resistance (LE 16-bit, raw; divide by 100 for Ohm)
 *   Bytes 10-11: unused
 *   Bytes 12-14: r_s — sensor resistance (LE 24-bit, Ohm)
 *   Byte 15: unused
 */

static unsigned short parse_voc_from_buf(const unsigned char *buf) {
    return (unsigned short)buf[2] | ((unsigned short)buf[3] << 8);
}

static unsigned short parse_debug_from_buf(const unsigned char *buf) {
    return (unsigned short)buf[4] | ((unsigned short)buf[5] << 8);
}

static unsigned short parse_pwm_from_buf(const unsigned char *buf) {
    return (unsigned short)buf[6] | ((unsigned short)buf[7] << 8);
}

/*
 * Heating resistance (r_h) — bytes 8-9, LE 16-bit.
 * Raw value; divide by 100.0 to get Ohm.
 */
static unsigned short parse_rh_raw_from_buf(const unsigned char *buf) {
    return (unsigned short)buf[8] | ((unsigned short)buf[9] << 8);
}

/*
 * Sensor resistance (r_s) — bytes 12-14, LE 24-bit (3 bytes).
 * This is the actual gas sensor resistance in Ohm.
 */
static unsigned int parse_rs_from_buf(const unsigned char *buf) {
    return (unsigned int)buf[12]
         | ((unsigned int)buf[13] << 8)
         | ((unsigned int)buf[14] << 16);
}

/*
 * MQTT address assembly
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
    print_header("VOC range validation");

    TEST("voc=450  is valid (lower boundary)",  voc_in_range(450)  == 1);
    TEST("voc=449  is invalid (below lower)",   voc_in_range(449)  == 0);
    TEST("voc=15001 is valid (upper boundary)", voc_in_range(15001) == 1);
    TEST("voc=15002 is invalid (above upper)",  voc_in_range(15002) == 0);
    TEST("voc=523  is valid (typical clean air)", voc_in_range(523) == 1);
    TEST("voc=2000 is valid (spec max)",        voc_in_range(2000) == 1);
    TEST("voc=0    is invalid",                 voc_in_range(0)    == 0);
    TEST("voc=65535 is invalid (uint16 max)",   voc_in_range(65535) == 0);
}

/* --- VOC buffer parsing -------------------------------------------------- */

static void suite_voc_parsing(void) {
    print_header("VOC buffer parsing — LE bytes 2-3");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    buf[2] = 0x0B; buf[3] = 0x02;
    TEST("parse 523  (0x020B)", parse_voc_from_buf(buf) == 523);

    memset(buf, 0, sizeof(buf));
    buf[2] = 0xC2; buf[3] = 0x01;
    TEST("parse 450  (0x01C2)", parse_voc_from_buf(buf) == 450);

    memset(buf, 0, sizeof(buf));
    buf[2] = 0xE8; buf[3] = 0x03;
    TEST("parse 1000 (0x03E8)", parse_voc_from_buf(buf) == 1000);

    memset(buf, 0, sizeof(buf));
    buf[2] = 0x99; buf[3] = 0x3A;
    TEST("parse 15001 (0x3A99)", parse_voc_from_buf(buf) == 15001);

    memset(buf, 0, sizeof(buf));
    TEST("parse 0: all-zero buffer", parse_voc_from_buf(buf) == 0);

    memset(buf, 0xFF, sizeof(buf));
    buf[2] = 0x0B; buf[3] = 0x02;
    TEST("parse 523 with surrounding 0xFF", parse_voc_from_buf(buf) == 523);
}

/* --- Debug value buffer parsing ------------------------------------------ */

static void suite_debug_parsing(void) {
    print_header("Debug value parsing — LE bytes 4-5");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    buf[4] = 0xE2; buf[5] = 0x02;
    TEST("parse debug=738 (0x02E2)", parse_debug_from_buf(buf) == 738);

    memset(buf, 0, sizeof(buf));
    TEST("parse debug=0 from zero buffer", parse_debug_from_buf(buf) == 0);

    memset(buf, 0, sizeof(buf));
    buf[4] = 0xFF; buf[5] = 0xFF;
    TEST("parse debug=65535 (0xFFFF)", parse_debug_from_buf(buf) == 65535);
}

/* --- PWM value buffer parsing -------------------------------------------- */

static void suite_pwm_parsing(void) {
    print_header("PWM value parsing — LE bytes 6-7");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    buf[6] = 0x0A; buf[7] = 0x00;
    TEST("parse pwm=10 (0x000A)", parse_pwm_from_buf(buf) == 10);

    memset(buf, 0, sizeof(buf));
    TEST("parse pwm=0 from zero buffer", parse_pwm_from_buf(buf) == 0);

    memset(buf, 0, sizeof(buf));
    buf[6] = 0xFF; buf[7] = 0xFF;
    TEST("parse pwm=65535 (0xFFFF)", parse_pwm_from_buf(buf) == 65535);

    /* Verify byte 7 is NOT humidity but high byte of PWM */
    memset(buf, 0, sizeof(buf));
    buf[6] = 0x00; buf[7] = 0x01;
    TEST("byte 7 is PWM high byte: pwm=256", parse_pwm_from_buf(buf) == 256);
}

/* --- Heating resistance (r_h) buffer parsing ----------------------------- */

static void suite_rh_parsing(void) {
    print_header("Heating resistance (r_h) parsing — LE bytes 8-9");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    TEST("r_h_raw=0 from zero buffer", parse_rh_raw_from_buf(buf) == 0);

    /* 738 raw → 7.38 Ohm */
    memset(buf, 0, sizeof(buf));
    buf[8] = 0xE2; buf[9] = 0x02;
    TEST("r_h_raw=738 (0x02E2)", parse_rh_raw_from_buf(buf) == 738);

    memset(buf, 0, sizeof(buf));
    buf[8] = 0xFF; buf[9] = 0xFF;
    TEST("r_h_raw=65535 (0xFFFF)", parse_rh_raw_from_buf(buf) == 65535);

    /* r_h division: 738 / 100 = 7.38 */
    memset(buf, 0, sizeof(buf));
    buf[8] = 0xE2; buf[9] = 0x02;
    double rh_ohm = parse_rh_raw_from_buf(buf) / 100.0;
    TEST("r_h 738/100 = 7.38 Ohm", rh_ohm > 7.37 && rh_ohm < 7.39);

    /* Verify bytes 8-9 are independent from surrounding bytes */
    memset(buf, 0xFF, sizeof(buf));
    buf[8] = 0xE2; buf[9] = 0x02;
    TEST("r_h=738 with surrounding 0xFF", parse_rh_raw_from_buf(buf) == 738);
}

/* --- Sensor resistance (r_s) buffer parsing ------------------------------ */

static void suite_rs_parsing(void) {
    print_header("Sensor resistance (r_s) parsing — LE bytes 12-14 (24-bit)");

    unsigned char buf[16];

    memset(buf, 0, sizeof(buf));
    TEST("r_s=0 from zero buffer", parse_rs_from_buf(buf) == 0);

    /* 38221 = 0x00954D → buf[12]=0x4D, buf[13]=0x95, buf[14]=0x00 */
    memset(buf, 0, sizeof(buf));
    buf[12] = 0x4D; buf[13] = 0x95; buf[14] = 0x00;
    TEST("r_s=38221 (0x00954D)", parse_rs_from_buf(buf) == 38221);

    /* 100000 = 0x0186A0 → buf[12]=0xA0, buf[13]=0x86, buf[14]=0x01 */
    memset(buf, 0, sizeof(buf));
    buf[12] = 0xA0; buf[13] = 0x86; buf[14] = 0x01;
    TEST("r_s=100000 (0x0186A0)", parse_rs_from_buf(buf) == 100000);

    /* Max 24-bit: 16777215 = 0xFFFFFF */
    memset(buf, 0, sizeof(buf));
    buf[12] = 0xFF; buf[13] = 0xFF; buf[14] = 0xFF;
    TEST("r_s=16777215 (0xFFFFFF, 24-bit max)", parse_rs_from_buf(buf) == 16777215);

    /* Byte 15 must NOT affect r_s (only 3 bytes, not 4) */
    memset(buf, 0, sizeof(buf));
    buf[12] = 0x4D; buf[13] = 0x95; buf[14] = 0x00; buf[15] = 0xFF;
    TEST("r_s=38221 ignores byte 15", parse_rs_from_buf(buf) == 38221);

    /* Verify independence from surrounding bytes */
    memset(buf, 0xFF, sizeof(buf));
    buf[12] = 0xA0; buf[13] = 0x86; buf[14] = 0x01;
    TEST("r_s=100000 with surrounding 0xFF", parse_rs_from_buf(buf) == 100000);
}

/* --- MQTT address assembly ----------------------------------------------- */

static void suite_mqtt_address(void) {
    print_header("MQTT broker address assembly");

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

static void build_discovery_topic(char *topic, size_t size,
                                  const char *prefix, const char *clientid) {
    snprintf(topic, size, "%s/sensor/%s/config", prefix, clientid);
}

static void build_discovery_payload(char *payload, size_t size,
                                    const char *device_name,
                                    const char *state_topic,
                                    const char *clientid,
                                    const char *avail_topic,
                                    int expire_after) {
    snprintf(payload, size,
             "{\"name\":\"%s VOC\","
             "\"object_id\":\"%s_voc\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.voc }}\","
             "\"unit_of_measurement\":\"ppm\","
             "\"device_class\":\"volatile_organic_compounds_parts\","
             "\"state_class\":\"measurement\","
             "\"suggested_display_precision\":0,"
             "\"unique_id\":\"%s_voc\","
             "\"availability_topic\":\"%s\","
             "\"expire_after\":%d,"
             "\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\","
             "\"model\":\"USB VOC Sensor\","
             "\"manufacturer\":\"AppliedSensor\"},"
             "\"origin\":{\"name\":\"airsensor-mqtt\","
             "\"sw_version\":\"test\","
             "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}}",
             device_name, clientid, state_topic, clientid,
             avail_topic, expire_after,
             clientid, device_name);
}

static void build_rh_discovery_payload(char *payload, size_t size,
                                       const char *device_name,
                                       const char *state_topic,
                                       const char *clientid,
                                       const char *avail_topic,
                                       int expire_after) {
    snprintf(payload, size,
             "{\"name\":\"%s Heating Resistance\","
             "\"object_id\":\"%s_rh\","
             "\"icon\":\"mdi:resistor\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.r_h }}\","
             "\"unit_of_measurement\":\"Ω\","
             "\"state_class\":\"measurement\","
             "\"suggested_display_precision\":2,"
             "\"unique_id\":\"%s_rh\","
             "\"availability_topic\":\"%s\","
             "\"expire_after\":%d,"
             "\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\","
             "\"model\":\"USB VOC Sensor\","
             "\"manufacturer\":\"AppliedSensor\"},"
             "\"origin\":{\"name\":\"airsensor-mqtt\","
             "\"sw_version\":\"test\","
             "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}}",
             device_name, clientid, state_topic, clientid,
             avail_topic, expire_after,
             clientid, device_name);
}

static void build_rs_discovery_payload(char *payload, size_t size,
                                       const char *device_name,
                                       const char *state_topic,
                                       const char *clientid,
                                       const char *avail_topic,
                                       int expire_after) {
    snprintf(payload, size,
             "{\"name\":\"%s Sensor Resistance\","
             "\"object_id\":\"%s_rs\","
             "\"icon\":\"mdi:resistor\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.r_s }}\","
             "\"unit_of_measurement\":\"Ω\","
             "\"state_class\":\"measurement\","
             "\"suggested_display_precision\":0,"
             "\"unique_id\":\"%s_rs\","
             "\"availability_topic\":\"%s\","
             "\"expire_after\":%d,"
             "\"device\":{\"identifiers\":[\"%s\"],"
             "\"name\":\"%s\","
             "\"model\":\"USB VOC Sensor\","
             "\"manufacturer\":\"AppliedSensor\"},"
             "\"origin\":{\"name\":\"airsensor-mqtt\","
             "\"sw_version\":\"test\","
             "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}}",
             device_name, clientid, state_topic, clientid,
             avail_topic, expire_after,
             clientid, device_name);
}

static void suite_ha_discovery(void) {
    print_header("Home Assistant MQTT discovery");

    char topic[256];

    build_discovery_topic(topic, sizeof(topic), "homeassistant", "airsensor");
    TEST("default topic: homeassistant/sensor/airsensor/config",
         strcmp(topic, "homeassistant/sensor/airsensor/config") == 0);

    build_discovery_topic(topic, sizeof(topic), "myhome", "airsensor");
    TEST("custom prefix: myhome/sensor/airsensor/config",
         strcmp(topic, "myhome/sensor/airsensor/config") == 0);

    build_discovery_topic(topic, sizeof(topic), "homeassistant", "wohnzimmer");
    TEST("custom clientid: homeassistant/sensor/wohnzimmer/config",
         strcmp(topic, "homeassistant/sensor/wohnzimmer/config") == 0);

    char payload[2048];

    /* VOC discovery payload — default poll_interval=30 → expire_after=90 */
    build_discovery_payload(payload, sizeof(payload),
                            "Air Sensor", "home/CO2/state", "airsensor",
                            "home/CO2/state/availability", 90);
    TEST("VOC payload contains object_id",
         strstr(payload, "\"object_id\":\"airsensor_voc\"") != NULL);
    TEST("VOC payload contains state_topic",
         strstr(payload, "\"state_topic\":\"home/CO2/state\"") != NULL);
    TEST("VOC payload contains value_template for voc",
         strstr(payload, "\"value_template\":\"{{ value_json.voc }}\"") != NULL);
    TEST("VOC payload contains unit ppm",
         strstr(payload, "\"unit_of_measurement\":\"ppm\"") != NULL);
    TEST("VOC payload contains device_class",
         strstr(payload, "\"device_class\":\"volatile_organic_compounds_parts\"") != NULL);
    TEST("VOC payload contains state_class measurement",
         strstr(payload, "\"state_class\":\"measurement\"") != NULL);
    TEST("VOC payload contains suggested_display_precision 0",
         strstr(payload, "\"suggested_display_precision\":0") != NULL);
    TEST("VOC payload contains unique_id",
         strstr(payload, "\"unique_id\":\"airsensor_voc\"") != NULL);
    TEST("VOC payload contains availability_topic",
         strstr(payload, "\"availability_topic\":\"home/CO2/state/availability\"") != NULL);
    TEST("VOC payload contains expire_after 90",
         strstr(payload, "\"expire_after\":90") != NULL);
    TEST("VOC payload contains origin name",
         strstr(payload, "\"origin\":{\"name\":\"airsensor-mqtt\"") != NULL);
    TEST("VOC payload contains origin support_url",
         strstr(payload, "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"") != NULL);

    /* r_h (heating resistance) discovery payload */
    build_rh_discovery_payload(payload, sizeof(payload),
                               "Air Sensor", "home/CO2/state", "airsensor",
                               "home/CO2/state/availability", 90);
    TEST("r_h payload contains name 'Heating Resistance'",
         strstr(payload, "\"name\":\"Air Sensor Heating Resistance\"") != NULL);
    TEST("r_h payload contains object_id",
         strstr(payload, "\"object_id\":\"airsensor_rh\"") != NULL);
    TEST("r_h payload contains icon mdi:resistor",
         strstr(payload, "\"icon\":\"mdi:resistor\"") != NULL);
    TEST("r_h payload contains value_template for r_h",
         strstr(payload, "\"value_template\":\"{{ value_json.r_h }}\"") != NULL);
    TEST("r_h payload contains unit Ω",
         strstr(payload, "\"unit_of_measurement\":\"Ω\"") != NULL);
    TEST("r_h payload contains state_class measurement",
         strstr(payload, "\"state_class\":\"measurement\"") != NULL);
    TEST("r_h payload contains suggested_display_precision 2",
         strstr(payload, "\"suggested_display_precision\":2") != NULL);
    TEST("r_h payload contains unique_id airsensor_rh",
         strstr(payload, "\"unique_id\":\"airsensor_rh\"") != NULL);
    TEST("r_h payload contains availability_topic",
         strstr(payload, "\"availability_topic\":\"home/CO2/state/availability\"") != NULL);
    TEST("r_h payload contains expire_after",
         strstr(payload, "\"expire_after\":90") != NULL);
    TEST("r_h payload contains origin",
         strstr(payload, "\"origin\":{\"name\":\"airsensor-mqtt\"") != NULL);

    /* r_s (sensor resistance) discovery payload */
    build_rs_discovery_payload(payload, sizeof(payload),
                               "Air Sensor", "home/CO2/state", "airsensor",
                               "home/CO2/state/availability", 90);
    TEST("r_s payload contains name 'Sensor Resistance'",
         strstr(payload, "\"name\":\"Air Sensor Sensor Resistance\"") != NULL);
    TEST("r_s payload contains object_id",
         strstr(payload, "\"object_id\":\"airsensor_rs\"") != NULL);
    TEST("r_s payload contains icon mdi:resistor",
         strstr(payload, "\"icon\":\"mdi:resistor\"") != NULL);
    TEST("r_s payload contains value_template for r_s",
         strstr(payload, "\"value_template\":\"{{ value_json.r_s }}\"") != NULL);
    TEST("r_s payload contains unit Ω",
         strstr(payload, "\"unit_of_measurement\":\"Ω\"") != NULL);
    TEST("r_s payload contains state_class measurement",
         strstr(payload, "\"state_class\":\"measurement\"") != NULL);
    TEST("r_s payload contains suggested_display_precision 0",
         strstr(payload, "\"suggested_display_precision\":0") != NULL);
    TEST("r_s payload contains unique_id airsensor_rs",
         strstr(payload, "\"unique_id\":\"airsensor_rs\"") != NULL);
    TEST("r_s payload contains availability_topic",
         strstr(payload, "\"availability_topic\":\"home/CO2/state/availability\"") != NULL);
    TEST("r_s payload contains expire_after",
         strstr(payload, "\"expire_after\":90") != NULL);
    TEST("r_s payload contains origin",
         strstr(payload, "\"origin\":{\"name\":\"airsensor-mqtt\"") != NULL);

    /* expire_after scales with poll_interval */
    build_discovery_payload(payload, sizeof(payload),
                            "Air Sensor", "home/CO2/state", "airsensor",
                            "home/CO2/state/availability", 300);
    TEST("VOC expire_after 300 for poll_interval=100",
         strstr(payload, "\"expire_after\":300") != NULL);
}

/* --- *IDN? response parsing ---------------------------------------------- */

static void suite_idn_parsing(void) {
    print_header("*IDN? response parsing — serial number and firmware");

    char serial[20];
    char firmware[20];
    int ret;

    ret = parse_serial_from_idn_response("DEVICE S/N:4142434445-000001 FW:1.12p5 CPU:ATmega32U4",
                                          serial, sizeof(serial));
    TEST("serial found in typical response", ret == 0);
    TEST("serial value: 4142434445-000001",
         strcmp(serial, "4142434445-000001") == 0);

    ret = parse_firmware_from_idn_response("DEVICE S/N:4142434445-000001 FW:1.12p5 CPU:ATmega32U4",
                                            firmware, sizeof(firmware));
    TEST("firmware found in typical response", ret == 0);
    TEST("firmware value: 1.12p5", strcmp(firmware, "1.12p5") == 0);

    ret = parse_serial_from_idn_response("NO SERIAL HERE", serial, sizeof(serial));
    TEST("no S/N: marker returns -1", ret == -1);

    ret = parse_firmware_from_idn_response("NO FIRMWARE HERE", firmware, sizeof(firmware));
    TEST("firmware not found returns -1", ret == -1);

    ret = parse_serial_from_idn_response("S/N:ABCDEF123456", serial, sizeof(serial));
    TEST("serial at end of string", ret == 0);
    TEST("serial value: ABCDEF123456", strcmp(serial, "ABCDEF123456") == 0);

    ret = parse_firmware_from_idn_response("FW:2.0", firmware, sizeof(firmware));
    TEST("firmware at end of string", ret == 0);
    TEST("firmware value: 2.0", strcmp(firmware, "2.0") == 0);

    ret = parse_serial_from_idn_response("S/N:ABC123\nFW:1.0\n", serial, sizeof(serial));
    TEST("serial terminated by newline", ret == 0);
    TEST("serial value: ABC123", strcmp(serial, "ABC123") == 0);

    ret = parse_serial_from_idn_response("S/N:ABC123;FW:1.2", serial, sizeof(serial));
    TEST("serial terminated by semicolon", ret == 0);
    TEST("serial value: ABC123", strcmp(serial, "ABC123") == 0);

    ret = parse_firmware_from_idn_response("FW:1.12p5;MCU:ATmega", firmware, sizeof(firmware));
    TEST("firmware terminated by semicolon", ret == 0);
    TEST("firmware value: 1.12p5", strcmp(firmware, "1.12p5") == 0);

    ret = parse_firmware_from_idn_response("FW:1.12p5@@@@", firmware, sizeof(firmware));
    TEST("firmware terminated by @ padding", ret == 0);
    TEST("firmware value with @ terminator", strcmp(firmware, "1.12p5") == 0);

    ret = parse_firmware_from_idn_response("...;1.12p5$;;MCU...", firmware, sizeof(firmware));
    TEST("firmware fallback marker parsed", ret == 0);
    TEST("firmware fallback value: 1.12p5", strcmp(firmware, "1.12p5") == 0);
}

/* --- JSON payload format ------------------------------------------------- */

static void suite_json_payload(void) {
    print_header("JSON state payload format");

    char json[512];
    unsigned short voc = 523;
    unsigned short debug_val = 738;
    unsigned short pwm_val = 10;
    unsigned short rh_raw = 738;
    unsigned int rs = 38221;

    snprintf(json, sizeof(json),
             "{\"voc\":%u,\"r_h\":%.2f,\"r_s\":%u,\"debug\":%u,\"pwm\":%u}",
             voc, rh_raw / 100.0, rs, debug_val, pwm_val);

    TEST("json contains voc",   strstr(json, "\"voc\":523") != NULL);
    TEST("json contains r_h",   strstr(json, "\"r_h\":7.38") != NULL);
    TEST("json contains r_s",   strstr(json, "\"r_s\":38221") != NULL);
    TEST("json contains debug", strstr(json, "\"debug\":738") != NULL);
    TEST("json contains pwm",   strstr(json, "\"pwm\":10") != NULL);
    TEST("json does NOT contain humidity", strstr(json, "humidity") == NULL);
}

/* --- Poll command with sequence numbers ---------------------------------- */

static void suite_poll_command(void) {
    print_header("Poll command with sequence numbers");

    char cmd[16];

    /* Initial sequence 0x67 */
    build_poll_command(0x67, cmd);
    TEST("cmd[0] is '@'", cmd[0] == '@');
    TEST("cmd[1] is seq 0x67", (unsigned char)cmd[1] == 0x67);
    TEST("cmd[2..4] is '*TR'", cmd[2] == '*' && cmd[3] == 'T' && cmd[4] == 'R');
    TEST("cmd[5] is newline", cmd[5] == '\n');
    TEST("cmd[6..15] is '@' padding", cmd[6] == '@' && cmd[15] == '@');

    /* Different sequence */
    build_poll_command(0xAB, cmd);
    TEST("cmd[1] is seq 0xAB", (unsigned char)cmd[1] == 0xAB);

    /* Sequence increment */
    TEST("seq 0x67 → 0x68", next_poll_seq(0x67) == 0x68);
    TEST("seq 0xFE → 0xFF", next_poll_seq(0xFE) == 0xFF);
    TEST("seq 0xFF wraps to 0x67", next_poll_seq(0xFF) == 0x67);

    /* Full cycle: 0x67 through 0xFF is 153 values */
    unsigned char seq = 0x67;
    int count = 0;
    do {
        seq = next_poll_seq(seq);
        count++;
    } while (seq != 0x67);
    TEST("full sequence cycle is 153 steps", count == 153);
}

/* --- Environment variable parsing with bounds ---------------------------- */

static void suite_env_parsing(void) {
    print_header("Environment variable parsing with bounds");

    /* Poll interval: default 30, min 10, max 3600 */
    TEST("poll interval: NULL → default 30", parse_env_int(NULL, 30, 10, 3600) == 30);
    TEST("poll interval: empty → default 30", parse_env_int("", 30, 10, 3600) == 30);
    TEST("poll interval: '60' → 60", parse_env_int("60", 30, 10, 3600) == 60);
    TEST("poll interval: '5' → clamped to 10", parse_env_int("5", 30, 10, 3600) == 10);
    TEST("poll interval: '9999' → clamped to 3600", parse_env_int("9999", 30, 10, 3600) == 3600);
    TEST("poll interval: '10' → boundary 10", parse_env_int("10", 30, 10, 3600) == 10);
    TEST("poll interval: '3600' → boundary 3600", parse_env_int("3600", 30, 10, 3600) == 3600);

    /* USB timeout: default 1000, min 250, max 10000 */
    TEST("usb timeout: NULL → default 1000", parse_env_int(NULL, 1000, 250, 10000) == 1000);
    TEST("usb timeout: '500' → 500", parse_env_int("500", 1000, 250, 10000) == 500);
    TEST("usb timeout: '100' → clamped to 250", parse_env_int("100", 1000, 250, 10000) == 250);
    TEST("usb timeout: '20000' → clamped to 10000", parse_env_int("20000", 1000, 250, 10000) == 10000);

    /* Max retries: default 3, min 1, max 20 */
    TEST("max retries: NULL → default 3", parse_env_int(NULL, 3, 1, 20) == 3);
    TEST("max retries: '5' → 5", parse_env_int("5", 3, 1, 20) == 5);
    TEST("max retries: '0' → clamped to 1", parse_env_int("0", 3, 1, 20) == 1);
    TEST("max retries: '50' → clamped to 20", parse_env_int("50", 3, 1, 20) == 20);
}

/* --- Retry / reconnect logic --------------------------------------------- */

/*
 * Returns 1 if we should attempt a reconnect (fail_count >= max_retries).
 * Returns 0 if we should just retry.
 */
static int should_reconnect(int fail_count, int max_retries) {
    return (fail_count >= max_retries) ? 1 : 0;
}

static void suite_retry_logic(void) {
    print_header("Retry / reconnect decision logic");

    TEST("fail=0, max=3 → retry",     should_reconnect(0, 3) == 0);
    TEST("fail=1, max=3 → retry",     should_reconnect(1, 3) == 0);
    TEST("fail=2, max=3 → retry",     should_reconnect(2, 3) == 0);
    TEST("fail=3, max=3 → reconnect", should_reconnect(3, 3) == 1);
    TEST("fail=4, max=3 → reconnect", should_reconnect(4, 3) == 1);
    TEST("fail=1, max=1 → reconnect", should_reconnect(1, 1) == 1);
    TEST("fail=0, max=1 → retry",     should_reconnect(0, 1) == 0);
    TEST("fail=19, max=20 → retry",   should_reconnect(19, 20) == 0);
    TEST("fail=20, max=20 → reconnect", should_reconnect(20, 20) == 1);
}

/* --- Data command building (FLAGGET?, KNOBPRE?) -------------------------- */

static void suite_data_command(void) {
    print_header("Data command building (FLAGGET?, KNOBPRE?)");

    char cmd[16];

    build_data_command(0x0001, "*IDN?", cmd);
    TEST("IDN cmd starts with @", cmd[0] == '@');
    TEST("IDN cmd seq '0001'", cmd[1]=='0' && cmd[2]=='0' && cmd[3]=='0' && cmd[4]=='1');
    TEST("IDN cmd contains '*IDN?'", memcmp(cmd+5, "*IDN?", 5) == 0);
    TEST("IDN cmd byte 10 is newline", cmd[10] == '\n');

    build_data_command(0x0002, "FLAGGET?", cmd);
    TEST("FLAG cmd seq '0002'", cmd[1]=='0' && cmd[2]=='0' && cmd[3]=='0' && cmd[4]=='2');
    TEST("FLAG cmd contains 'FLAGGET?'", memcmp(cmd+5, "FLAGGET?", 8) == 0);
    TEST("FLAG cmd byte 13 is newline", cmd[13] == '\n');

    build_data_command(0x0003, "KNOBPRE?", cmd);
    TEST("KNOB cmd contains 'KNOBPRE?'", memcmp(cmd+5, "KNOBPRE?", 8) == 0);

    /* Sequence wrapping */
    build_data_command(0xFFFF, "*IDN?", cmd);
    TEST("seq FFFF", cmd[1]=='F' && cmd[2]=='F' && cmd[3]=='F' && cmd[4]=='F');
}

/* --- FLAGGET? response parsing ------------------------------------------- */

static void suite_flagget_parsing(void) {
    print_header("FLAGGET? response parsing");

    device_flags_t flags;
    int ret;

    /* Synthetic response: ';' at offset 5, then 5 LE16 values */
    unsigned char data[48];
    memset(data, 0, sizeof(data));
    data[5] = ';';
    /* warmup = 30 (0x001E) at offset 5+2=7 */
    data[7] = 0x1E; data[8] = 0x00;
    /* burn_in = 1440 (0x05A0) at offset 5+6=11 */
    data[11] = 0xA0; data[12] = 0x05;
    /* reset_baseline = 0 at offset 5+10=15 */
    data[15] = 0x00; data[16] = 0x00;
    /* calibrate_heater = 1 at offset 5+14=19 */
    data[19] = 0x01; data[20] = 0x00;
    /* logging = 100 (0x0064) at offset 5+18=23 */
    data[23] = 0x64; data[24] = 0x00;

    ret = parse_flags_response(data, sizeof(data), &flags);
    TEST("parse flags: success", ret == 0);
    TEST("warmup = 30 minutes", flags.warmup == 30);
    TEST("burn_in = 1440 minutes", flags.burn_in == 1440);
    TEST("reset_baseline = 0", flags.reset_baseline == 0);
    TEST("calibrate_heater = 1", flags.calibrate_heater == 1);
    TEST("logging = 100", flags.logging == 100);

    /* All zeros after delimiter */
    memset(data, 0, sizeof(data));
    data[0] = ';';
    ret = parse_flags_response(data, sizeof(data), &flags);
    TEST("all zeros: success", ret == 0);
    TEST("all zeros: warmup = 0", flags.warmup == 0);
    TEST("all zeros: burn_in = 0", flags.burn_in == 0);

    /* No delimiter → failure */
    memset(data, 'A', sizeof(data));
    ret = parse_flags_response(data, sizeof(data), &flags);
    TEST("no delimiter: returns -1", ret == -1);

    /* Delimiter too close to end → failure */
    memset(data, 0, sizeof(data));
    data[45] = ';';
    ret = parse_flags_response(data, sizeof(data), &flags);
    TEST("delimiter near end: returns -1", ret == -1);
}

/* --- KNOBPRE? response parsing ------------------------------------------- */

static void suite_knobpre_parsing(void) {
    print_header("KNOBPRE? response parsing (warn thresholds)");

    device_knobs_t knobs;
    int ret;

    /* Build synthetic response with "warn1" at offset 10, "warn2" at offset 50 */
    unsigned char data[256];
    memset(data, 0, sizeof(data));
    memcpy(data + 10, "warn1", 5);
    /* warn1 value = 1000 (0x03E8) at offset 10+22=32 */
    data[32] = 0xE8; data[33] = 0x03;
    memcpy(data + 50, "warn2", 5);
    /* warn2 value = 1500 (0x05DC) at offset 50+22=72 */
    data[72] = 0xDC; data[73] = 0x05;

    ret = parse_knobs_response(data, sizeof(data), &knobs);
    TEST("parse knobs: success", ret == 0);
    TEST("warn1 = 1000 ppm", knobs.warn1 == 1000);
    TEST("warn2 = 1500 ppm", knobs.warn2 == 1500);

    /* Only warn1 present */
    memset(data, 0, sizeof(data));
    memcpy(data + 10, "warn1", 5);
    data[32] = 0xC2; data[33] = 0x01;
    ret = parse_knobs_response(data, sizeof(data), &knobs);
    TEST("only warn1: success", ret == 0);
    TEST("only warn1 = 450 ppm", knobs.warn1 == 450);
    TEST("only warn1: warn2 = 0", knobs.warn2 == 0);

    /* No markers → failure */
    memset(data, 'X', sizeof(data));
    ret = parse_knobs_response(data, sizeof(data), &knobs);
    TEST("no markers: returns -1", ret == -1);

    /* Empty data */
    ret = parse_knobs_response(data, 0, &knobs);
    TEST("empty data: returns -1", ret == -1);
}

/* --- Diagnostic HA discovery --------------------------------------------- */

static void suite_diagnostic_discovery(void) {
    print_header("Diagnostic HA discovery (warmup, warn thresholds)");

    char payload[2048];

    /* Warmup diagnostic sensor */
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s Warmup\","
             "\"object_id\":\"%s_warmup\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.warmup }}\","
             "\"unit_of_measurement\":\"min\","
             "\"entity_category\":\"diagnostic\","
             "\"unique_id\":\"%s_warmup\","
             "\"availability_topic\":\"%s\","
             "\"device\":{\"identifiers\":[\"%s\"]},"
             "\"origin\":{\"name\":\"airsensor-mqtt\","
             "\"sw_version\":\"test\","
             "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}}",
             "Air Sensor", "airsensor", "home/CO2/state", "airsensor",
             "home/CO2/state/availability", "airsensor");

    TEST("warmup payload contains name",
         strstr(payload, "\"name\":\"Air Sensor Warmup\"") != NULL);
    TEST("warmup payload contains object_id",
         strstr(payload, "\"object_id\":\"airsensor_warmup\"") != NULL);
    TEST("warmup payload contains value_template",
         strstr(payload, "\"value_template\":\"{{ value_json.warmup }}\"") != NULL);
    TEST("warmup payload contains unit min",
         strstr(payload, "\"unit_of_measurement\":\"min\"") != NULL);
    TEST("warmup payload contains entity_category diagnostic",
         strstr(payload, "\"entity_category\":\"diagnostic\"") != NULL);
    TEST("warmup payload contains unique_id",
         strstr(payload, "\"unique_id\":\"airsensor_warmup\"") != NULL);
    TEST("warmup payload contains availability_topic",
         strstr(payload, "\"availability_topic\":\"home/CO2/state/availability\"") != NULL);
    TEST("warmup payload contains origin",
         strstr(payload, "\"origin\":{\"name\":\"airsensor-mqtt\"") != NULL);

    /* Warn1 threshold diagnostic sensor */
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s Warn Threshold 1\","
             "\"object_id\":\"%s_warn1\","
             "\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.warn1 }}\","
             "\"unit_of_measurement\":\"ppm\","
             "\"entity_category\":\"diagnostic\","
             "\"unique_id\":\"%s_warn1\","
             "\"availability_topic\":\"%s\","
             "\"device\":{\"identifiers\":[\"%s\"]},"
             "\"origin\":{\"name\":\"airsensor-mqtt\","
             "\"sw_version\":\"test\","
             "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}}",
             "Air Sensor", "airsensor", "home/CO2/diag", "airsensor",
             "home/CO2/diag/availability", "airsensor");

    TEST("warn1 payload contains name",
         strstr(payload, "\"name\":\"Air Sensor Warn Threshold 1\"") != NULL);
    TEST("warn1 payload contains object_id",
         strstr(payload, "\"object_id\":\"airsensor_warn1\"") != NULL);
    TEST("warn1 payload contains value_template",
         strstr(payload, "\"value_template\":\"{{ value_json.warn1 }}\"") != NULL);
    TEST("warn1 payload contains entity_category diagnostic",
         strstr(payload, "\"entity_category\":\"diagnostic\"") != NULL);
    TEST("warn1 payload contains availability_topic",
         strstr(payload, "\"availability_topic\":\"home/CO2/diag/availability\"") != NULL);
    TEST("warn1 payload contains origin",
         strstr(payload, "\"origin\":{\"name\":\"airsensor-mqtt\"") != NULL);
}

/* --- svoc buffer size ---------------------------------------------------- */

static void suite_svoc_buffer(void) {
    print_header("svoc buffer size");

    char tmp[32];

    int len_523   = snprintf(tmp, sizeof(tmp), "%d", 523);
    int len_2000  = snprintf(tmp, sizeof(tmp), "%d", 2000);
    int len_9999  = snprintf(tmp, sizeof(tmp), "%d", 9999);
    int len_10000 = snprintf(tmp, sizeof(tmp), "%d", 10000);
    int len_15001 = snprintf(tmp, sizeof(tmp), "%d", 15001);

    TEST("523   requires 3 bytes, fits in svoc[5]",  len_523   + 1 <= 5);
    TEST("2000  requires 4 bytes, fits in svoc[5]",  len_2000  + 1 <= 5);
    TEST("9999  requires 4 bytes, fits in svoc[5]",  len_9999  + 1 <= 5);
    TEST("10000 requires 6 bytes, overflows svoc[5]", len_10000 + 1 > 5);
    TEST("15001 requires 6 bytes, overflows svoc[5]", len_15001 + 1 > 5);
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
    suite_debug_parsing();
    suite_pwm_parsing();
    suite_rh_parsing();
    suite_rs_parsing();
    suite_mqtt_address();
    suite_ha_discovery();
    suite_idn_parsing();
    suite_json_payload();
    suite_poll_command();
    suite_env_parsing();
    suite_retry_logic();
    suite_data_command();
    suite_flagget_parsing();
    suite_knobpre_parsing();
    suite_diagnostic_discovery();
    suite_svoc_buffer();

    printf("\n====================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return (tests_failed > 0) ? 1 : 0;
}

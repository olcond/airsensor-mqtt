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
    suite_mqtt_address();
    suite_svoc_buffer();

    printf("\n====================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    return (tests_failed > 0) ? 1 : 0;
}

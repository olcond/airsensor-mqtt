/*
 * airsensor.h — Pure-logic functions and types shared between airsensor.c
 * and the unit-test suite.
 *
 * All functions are declared static inline so this header can be included
 * in multiple translation units without violating the one-definition rule.
 *
 * No USB, MQTT, or OS I/O happens here.
 */

#ifndef AIRSENSOR_H
#define AIRSENSOR_H

#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define APP_VERSION "1.0.2"

/* --------------------------------------------------------------------------
 * Device data types
 * -------------------------------------------------------------------------- */

typedef struct {
    unsigned short warmup;
    unsigned short burn_in;
    unsigned short reset_baseline;
    unsigned short calibrate_heater;
    unsigned short logging;
} device_flags_t;

typedef struct {
    unsigned short warn1;
    unsigned short warn2;
} device_knobs_t;

/* --------------------------------------------------------------------------
 * Environment helpers
 * -------------------------------------------------------------------------- */

/*
 * Parse an integer from a string with default and clamping.
 */
static inline int parse_env_int(const char *val, int default_val, int min_val, int max_val) {
    if (!val || val[0] == '\0') return default_val;
    int v = atoi(val);
    if (v < min_val) return min_val;
    if (v > max_val) return max_val;
    return v;
}

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *broker;
    const char *port;
    const char *clientid;
    const char *topic;
    const char *ha_prefix;
    const char *ha_device_name;
    const char *mqtt_username;
    const char *mqtt_password;
    int poll_interval;
    int usb_timeout;
    int max_retries;
    int print_voc_only;
    int one_read;
    int tls;
} config_t;

static inline void config_init_from_env(config_t *cfg) {
    cfg->broker = getenv("MQTT_BROKERNAME");
    if (!cfg->broker) cfg->broker = "127.0.0.1";
    cfg->port = getenv("MQTT_PORT");
    if (!cfg->port) cfg->port = "1883";
    cfg->clientid = getenv("MQTT_CLIENTID");
    if (!cfg->clientid) cfg->clientid = "airsensor";
    cfg->topic = getenv("MQTT_TOPIC");
    if (!cfg->topic) cfg->topic = "home/CO2/voc";
    cfg->ha_prefix = getenv("HA_DISCOVERY_PREFIX");
    if (!cfg->ha_prefix) cfg->ha_prefix = "homeassistant";
    cfg->ha_device_name = getenv("HA_DEVICE_NAME");
    if (!cfg->ha_device_name) cfg->ha_device_name = "Air Sensor";
    cfg->mqtt_username = getenv("MQTT_USERNAME");
    cfg->mqtt_password = getenv("MQTT_PASSWORD");
    cfg->poll_interval = parse_env_int(getenv("POLL_INTERVAL"), 30, 10, 3600);
    cfg->usb_timeout = parse_env_int(getenv("USB_TIMEOUT"), 1000, 250, 10000);
    cfg->max_retries = parse_env_int(getenv("MAX_RETRIES"), 3, 1, 20);
    cfg->print_voc_only = 0;
    cfg->one_read = 0;
    const char *tls_env = getenv("MQTT_TLS");
    cfg->tls = (tls_env && (strcmp(tls_env, "1") == 0 || strcmp(tls_env, "true") == 0)) ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * VOC range validation
 * -------------------------------------------------------------------------- */

static inline int voc_in_range(unsigned short voc) {
    return (voc >= 450 && voc <= 15001);
}

/* --------------------------------------------------------------------------
 * *IDN? response parsing
 * -------------------------------------------------------------------------- */

/*
 * Return non-zero if c is a field delimiter in an *IDN? response.
 * Delimiters: space, newline, carriage-return, semicolon, at-sign.
 */
static inline int is_idn_delim(char c) {
    return (c == ' ' || c == '\n' || c == '\r' || c == ';' || c == '@');
}

/*
 * Parse the serial number from an *IDN? response string.
 * Looks for "S/N:" and extracts characters until the next delimiter or
 * end of string.  Writes a null-terminated result into out/out_size.
 * Returns 0 on success, -1 if the marker is not found.
 */
static inline int parse_serial_from_idn_response(const char *response,
                                                  char *out, size_t out_size) {
    const char *pos = strstr(response, "S/N:");
    if (!pos) return -1;
    pos += 4;
    size_t i = 0;
    while (pos[i] && !is_idn_delim(pos[i]) && i < out_size - 1) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

/*
 * Parse the firmware version from an *IDN? response string.
 * Primary: looks for "FW:" and extracts characters until the next
 * delimiter or end of string.
 * Fallback (FHEM-style responses without "FW:" marker): parse the
 * token between the first ';' and "$;;MCU", stripping a trailing '$'.
 * Returns 0 on success, -1 if no version can be extracted.
 */
static inline int parse_firmware_from_idn_response(const char *response,
                                                    char *out, size_t out_size) {
    /* Primary path: "FW:" marker */
    const char *pos = strstr(response, "FW:");
    if (pos) {
        pos += 3;
        size_t i = 0;
        while (pos[i] && !is_idn_delim(pos[i]) && i < out_size - 1) {
            out[i] = pos[i];
            i++;
        }
        out[i] = '\0';
        return 0;
    }

    /* Fallback path: token between first ';' and "$;;MCU" */
    const char *semi = strchr(response, ';');
    if (!semi) return -1;
    semi++;  /* skip the ';' */
    const char *end = strstr(semi, "$;;MCU");
    if (!end) return -1;
    /* strip trailing '$' before "$;;MCU" if present */
    while (end > semi && *(end - 1) == '$')
        end--;
    size_t len = (size_t)(end - semi);
    if (len == 0 || len >= out_size) return -1;
    memcpy(out, semi, len);
    out[len] = '\0';
    return 0;
}

/* --------------------------------------------------------------------------
 * Poll command building
 * -------------------------------------------------------------------------- */

static inline unsigned char next_poll_seq(unsigned char current) {
    return (current < 0xFF) ? (unsigned char)(current + 1) : 0x67;
}

static inline void build_poll_command(unsigned char seq, char *cmd) {
    cmd[0] = '@';
    cmd[1] = (char)seq;
    cmd[2] = '*'; cmd[3] = 'T'; cmd[4] = 'R';
    cmd[5] = '\n';
    for (int i = 6; i < 16; i++) cmd[i] = '@';
}

/* --------------------------------------------------------------------------
 * Data query command building
 * -------------------------------------------------------------------------- */

/*
 * Build a data query command with 4-hex-digit sequence number.
 * Format: "@" + seq4(4 hex) + reqstr + "\n" + "@" padding → 16 bytes
 */
static inline void build_data_command(unsigned short seq4, const char *reqstr, char *cmd) {
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "@%04X%s\n@@@@@@@@@@@@@@@@", seq4, reqstr);
    memcpy(cmd, tmp, 16);
}

/* --------------------------------------------------------------------------
 * Device response parsing
 * -------------------------------------------------------------------------- */

/*
 * Parse FLAGGET? response.
 * Finds ';' delimiter, then reads 5 LE16 values at offsets +2, +6, +10, +14, +18.
 */
static inline int parse_flags_response(const unsigned char *data, size_t len, device_flags_t *flags) {
    const unsigned char *semi = memchr(data, ';', len);
    if (!semi) return -1;
    size_t offset = (size_t)(semi - data);
    if (offset + 20 > len) return -1;
    flags->warmup           = semi[2]  | ((unsigned short)semi[3]  << 8);
    flags->burn_in          = semi[6]  | ((unsigned short)semi[7]  << 8);
    flags->reset_baseline   = semi[10] | ((unsigned short)semi[11] << 8);
    flags->calibrate_heater = semi[14] | ((unsigned short)semi[15] << 8);
    flags->logging          = semi[18] | ((unsigned short)semi[19] << 8);
    return 0;
}

/*
 * Parse KNOBPRE? response for warn thresholds.
 * Searches for "warn1"/"warn2" markers and reads LE16 at marker+22.
 */
static inline int parse_knobs_response(const unsigned char *data, size_t len, device_knobs_t *knobs) {
    knobs->warn1 = 0;
    knobs->warn2 = 0;
    int found = 0;
    for (size_t i = 0; i + 24 <= len; i++) {
        if (memcmp(data + i, "warn1", 5) == 0 && i + 24 <= len) {
            knobs->warn1 = data[i+22] | ((unsigned short)data[i+23] << 8);
            found = 1;
        }
        if (memcmp(data + i, "warn2", 5) == 0 && i + 24 <= len) {
            knobs->warn2 = data[i+22] | ((unsigned short)data[i+23] << 8);
            found = 1;
        }
    }
    return found ? 0 : -1;
}

/* --------------------------------------------------------------------------
 * Logging macros
 * -------------------------------------------------------------------------- */

#include <time.h>

#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_DEBUG 2

#ifndef AIRSENSOR_TEST
extern int log_level;
#endif

#define LOG(level, fmt, ...)                                         \
    do {                                                             \
        if (log_level >= (level)) {                                  \
            time_t _t = time(NULL);                                  \
            struct tm _tm;                                           \
            localtime_r(&_t, &_tm);                                  \
            const char *_label = (level) == LOG_LEVEL_ERROR ? "ERROR" \
                               : (level) == LOG_LEVEL_INFO  ? "INFO"  \
                               : "DEBUG";                             \
            fprintf(stderr, "%04d-%02d-%02d %02d:%02d:%02d [%s] " fmt "\n", \
                    _tm.tm_year + 1900, _tm.tm_mon + 1, _tm.tm_mday, \
                    _tm.tm_hour, _tm.tm_min, _tm.tm_sec,             \
                    _label, ##__VA_ARGS__);                          \
        }                                                            \
    } while (0)

#define LOG_ERROR(fmt, ...) LOG(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  LOG(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

#endif /* AIRSENSOR_H */

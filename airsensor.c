/*

    airsensor.c

    Original source: Rodric Yates http://code.google.com/p/airsensor-linux-usb/
    Modified from source: Ap15e (MiOS) http://wiki.micasaverde.com/index.php/CO2_Sensor
    Modified by Sebastian Sjoholm, sebastian.sjoholm@gmail.com

    This version created by Veit Olschinski, volschin@googlemail.com

    requirement:

    libusb-1.0 libpaho-mqtt3cs libpthread libssl libcrypto

    compile:

    gcc -o airsensor airsensor.c -lusb-1.0 -lpaho-mqtt3cs -lpthread -lssl -lcrypto

*/

#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libusb-1.0/libusb.h>
#include <endian.h>
#include <unistd.h>
#include <time.h>
#include "MQTTClient.h"
#include "airsensor.h"

#define QOS         1
#define TIMEOUT     10000L

int log_level = LOG_LEVEL_INFO;

MQTTClient client;
libusb_device_handle *devh;
char device_serial[20] = "";
char device_firmware[32] = "";
char device_manufacturer[64] = "";
char device_product[64] = "";
char g_avail_topic[256] = "";
static volatile sig_atomic_t shutdown_requested = 0;

/*
 * Poll command sequence number (FHEM CO20 protocol).
 * Range 0x67–0xFF, wraps back to 0x67.
 */
static unsigned char poll_seq = 0x67;

void help() {

    printf("AirSensor [options]\n");
    printf("Options:\n");
    printf("-d = debug printout\n");
    printf("-v = Print VOC value only, nothing returns if value out of range (450-2000)\n");
    printf("-o = One value and then exit\n");
    printf("-h = Help, this printout\n");
    exit(0);

}

void release_usb_device(int dummy) {
    (void)dummy;
    shutdown_requested = 1;
}

/*
 * Send a data query command and read N response chunks.
 * Returns total bytes read, or -1 on write failure.
 */
static int query_device_data(libusb_device_handle *handle, const char *reqstr,
                             unsigned char *resp_buf, int num_chunks, int timeout_ms) {
    static unsigned short data_seq = 1;
    char cmd[16];
    build_data_command(data_seq, reqstr, cmd);
    data_seq = (data_seq < 0xFFFF) ? (unsigned short)(data_seq + 1) : 1;

    int transferred;
    int ret = libusb_interrupt_transfer(handle, 0x02, (unsigned char*)cmd, 16, &transferred, timeout_ms);
    if (ret != 0 || transferred != 16) return -1;

    int total = 0;
    for (int i = 0; i < num_chunks; i++) {
        ret = libusb_interrupt_transfer(handle, 0x81, resp_buf + total, 16, &transferred, timeout_ms);
        if (ret == 0 && transferred > 0) total += transferred;
        else break;
    }
    return total;
}

/*
 * Send a Type 1 command (*IDN?) to query device identification.
 * Reads the response into resp_buf (null-terminated string).
 * Returns 0 on success, -1 on failure.
 */
static int query_device_id(libusb_device_handle *handle, char *resp_buf, size_t resp_size, int timeout_ms) {
    static unsigned short idn_seq = 1;
    char cmd[17];
    snprintf(cmd, sizeof(cmd), "@%04X*IDN?\n@@@@@", idn_seq);
    idn_seq = (idn_seq < 0xFFFF) ? (unsigned short)(idn_seq + 1) : 1;
    int transferred;
    int ret = libusb_interrupt_transfer(handle, 0x02, (unsigned char*)cmd, 16, &transferred, timeout_ms);
    if (ret != 0) return -1;

    size_t total = 0;
    unsigned char chunk[16];
    int attempts = 0;
    while (total < resp_size - 1 && attempts < 10) {
        ret = libusb_interrupt_transfer(handle, 0x81, chunk, 16, &transferred, timeout_ms);
        if (ret != 0 || transferred <= 0) break;
        int start = (attempts == 0) ? 1 : 0;
        for (int i = start; i < transferred && total < resp_size - 1; i++) {
            if (chunk[i] == '@') continue;
            resp_buf[total++] = (char)chunk[i];
        }
        attempts++;
    }
    resp_buf[total] = '\0';
    return (total > 0) ? 0 : -1;
}

typedef struct {
    const char *suffix;       /* e.g. "_rh", "_rs", "_warmup", "_warn1", "_warn2", or "_voc" for primary */
    const char *name;         /* "VOC", "Heating Resistance", etc. */
    const char *value_tpl;    /* "{{ value_json.voc }}", etc. */
    const char *unit;         /* "ppm", "Ω", "min", etc. */
    const char *device_class; /* NULL if none */
    const char *state_class;  /* "measurement" or NULL */
    const char *entity_cat;   /* "diagnostic" or NULL */
    const char *icon;         /* "mdi:resistor" or NULL */
    int precision;            /* suggested_display_precision, -1 to omit */
    int expire_after;         /* -1 to omit */
    const char *state_topic;
    const char *avail_topic;
} discovery_entity_t;

static void publish_discovery(MQTTClient client_handle,
                              const config_t *cfg,
                              const char *device_block,
                              const char *origin_block,
                              const discovery_entity_t *ent) {
    char topic[256];
    if (ent->suffix)
        snprintf(topic, sizeof(topic), "%s/sensor/%s%s/config",
                 cfg->ha_prefix, cfg->clientid, ent->suffix);
    else
        snprintf(topic, sizeof(topic), "%s/sensor/%s/config",
                 cfg->ha_prefix, cfg->clientid);

    char payload[1536];
    int pos = 0;
    pos += snprintf(payload + pos, sizeof(payload) - pos,
                    "{\"name\":\"%s %s\","
                    "\"object_id\":\"%s%s\","
                    "\"state_topic\":\"%s\","
                    "\"value_template\":\"%s\","
                    "\"unit_of_measurement\":\"%s\",",
                    cfg->ha_device_name, ent->name,
                    cfg->clientid, ent->suffix ? ent->suffix + 1 : "",
                    ent->state_topic, ent->value_tpl, ent->unit);

    if (ent->device_class)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"device_class\":\"%s\",", ent->device_class);
    if (ent->state_class)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"state_class\":\"%s\",", ent->state_class);
    if (ent->entity_cat)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"entity_category\":\"%s\",", ent->entity_cat);
    if (ent->icon)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"icon\":\"%s\",", ent->icon);
    if (ent->precision >= 0)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"suggested_display_precision\":%d,", ent->precision);
    if (ent->expire_after >= 0)
        pos += snprintf(payload + pos, sizeof(payload) - pos,
                        "\"expire_after\":%d,", ent->expire_after);

    snprintf(payload + pos, sizeof(payload) - pos,
                    "\"unique_id\":\"%s%s\","
                    "\"availability_topic\":\"%s\","
                    "%s,%s}",
                    cfg->clientid, ent->suffix ? ent->suffix : "_voc",
                    ent->avail_topic, device_block, origin_block);

    MQTTClient_message disc_msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken disc_token;
    disc_msg.payload = payload;
    disc_msg.payloadlen = (int)strlen(payload);
    disc_msg.qos = QOS;
    disc_msg.retained = 1;
    MQTTClient_publishMessage(client_handle, topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client_handle, disc_token, TIMEOUT);
}

static int init_mqtt(const config_t *cfg, char *avail_topic, size_t avail_size) {
    char address[256];
    const char *proto = cfg->tls ? "ssl" : "tcp";
    snprintf(address, sizeof(address), "%s://%s:%s", proto, cfg->broker, cfg->port);

    snprintf(avail_topic, avail_size, "%s/availability", cfg->topic);
    snprintf(g_avail_topic, sizeof(g_avail_topic), "%s", avail_topic);

    MQTTClient_create(&client, address, cfg->clientid,
        MQTTCLIENT_PERSISTENCE_NONE, NULL);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    conn_opts.keepAliveInterval = 70;
    conn_opts.cleansession = 1;
    conn_opts.username = cfg->mqtt_username;
    conn_opts.password = cfg->mqtt_password;

    will_opts.topicName = avail_topic;
    will_opts.message = "offline";
    will_opts.qos = QOS;
    will_opts.retained = 1;
    conn_opts.will = &will_opts;

    MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;
    if (cfg->tls) {
        ssl_opts.enableServerCertAuth = 1;
        conn_opts.ssl = &ssl_opts;
    }

    int rc = MQTTClient_connect(client, &conn_opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        LOG_ERROR("Failed to connect, return code %d", rc);
        exit(EXIT_FAILURE);
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    pubmsg.payload = "online";
    pubmsg.payloadlen = 6;
    pubmsg.qos = QOS;
    pubmsg.retained = 1;
    MQTTClient_publishMessage(client, avail_topic, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, TIMEOUT);

    return rc;
}

static libusb_device_handle* init_usb(int vendor, int product, int usb_timeout) {
    int ret;

    LOG_DEBUG("Init USB");
    libusb_init(NULL);

    libusb_device_handle *handle = NULL;
    int counter = 0;
    do {
        handle = libusb_open_device_with_vid_pid(NULL, (uint16_t)vendor, (uint16_t)product);

        if (handle) break;

        LOG_DEBUG("No device found, wait 10sec...");
        sleep(11);
        ++counter;

        if (counter == 10) {
            LOG_ERROR("Device not found");
            MQTTClient_disconnect(client, 10000);
            MQTTClient_destroy(&client);
            libusb_exit(NULL);
            exit(1);
        }
    } while (1);

    LOG_DEBUG("USB device found");

    libusb_device *usb_dev = libusb_get_device(handle);
    struct libusb_device_descriptor desc;
    libusb_get_device_descriptor(usb_dev, &desc);
    if (desc.iManufacturer) {
        libusb_get_string_descriptor_ascii(handle, desc.iManufacturer,
                                           (unsigned char*)device_manufacturer, sizeof(device_manufacturer));
    }
    if (desc.iProduct) {
        libusb_get_string_descriptor_ascii(handle, desc.iProduct,
                                           (unsigned char*)device_product, sizeof(device_product));
    }

    if (device_manufacturer[0])
        LOG_DEBUG("Manufacturer: %s", device_manufacturer);
    if (device_product[0])
        LOG_DEBUG("Product: %s", device_product);

    signal(SIGTERM, release_usb_device);
    signal(SIGINT,  release_usb_device);

    if (libusb_kernel_driver_active(handle, 0) == 1) {
        libusb_detach_kernel_driver(handle, 0);
    }

    ret = libusb_claim_interface(handle, 0);
    if (ret != 0) {
        LOG_ERROR("Claim failed with error: %d", ret);
        libusb_close(handle);
        MQTTClient_disconnect(client, 10000);
        MQTTClient_destroy(&client);
        libusb_exit(NULL);
        exit(1);
    }

    /* Flush any pending data */
    unsigned char flush_buf[16];
    int flush_transferred;
    libusb_interrupt_transfer(handle, 0x81, flush_buf, 16, &flush_transferred, usb_timeout);

    return handle;
}

static void query_device_info(libusb_device_handle *handle, int usb_timeout,
                              device_flags_t *flags, int *flags_valid,
                              device_knobs_t *knobs, int *knobs_valid) {
    unsigned char flush_buf[16];
    int flush_transferred;

    /* *IDN? query */
    char idn_response[256];
    if (query_device_id(handle, idn_response, sizeof(idn_response), usb_timeout) == 0) {
        parse_serial_from_idn_response(idn_response, device_serial, sizeof(device_serial));
        parse_firmware_from_idn_response(idn_response, device_firmware, sizeof(device_firmware));

        if (device_serial[0] == '\0') {
            size_t dest_idx = 0, j = 0;
            while (idn_response[j] && dest_idx < sizeof(device_serial) - 1) {
                char c = idn_response[j];
                if (!is_idn_delim(c))
                    device_serial[dest_idx++] = c;
                j++;
            }
            device_serial[dest_idx] = '\0';
        }
        if (device_serial[0])
            printf("Device serial: %s\n", device_serial);
        if (device_firmware[0] == '\0')
            snprintf(device_firmware, sizeof(device_firmware), "1.12p5 $Revision: 346");
        printf("Device firmware: %s\n", device_firmware);
        LOG_DEBUG("*IDN? query successful");
    } else {
        LOG_DEBUG("*IDN? query failed, continuing without device info");
    }
    libusb_interrupt_transfer(handle, 0x81, flush_buf, 16, &flush_transferred, usb_timeout);

    /* FLAGGET? query */
    memset(flags, 0, sizeof(*flags));
    *flags_valid = 0;
    {
        unsigned char flag_resp[48];
        int flag_len = query_device_data(handle, "FLAGGET?", flag_resp, 3, usb_timeout);
        if (flag_len > 0 && parse_flags_response(flag_resp, (size_t)flag_len, flags) == 0) {
            *flags_valid = 1;
            if (flags->warmup > 0)
                printf("Warmup: %d min remaining\n", flags->warmup);
            if (flags->burn_in > 0)
                printf("Burn-in: %d min remaining\n", flags->burn_in);
            LOG_DEBUG("FLAGGET? successful — warmup=%d, burn_in=%d, logging=%d",
                      flags->warmup, flags->burn_in, flags->logging);
        } else {
            LOG_DEBUG("FLAGGET? query failed");
        }
        libusb_interrupt_transfer(handle, 0x81, flush_buf, 16, &flush_transferred, usb_timeout);
    }

    /* KNOBPRE? query */
    memset(knobs, 0, sizeof(*knobs));
    *knobs_valid = 0;
    {
        unsigned char knob_resp[256];
        int knob_len = query_device_data(handle, "KNOBPRE?", knob_resp, 16, usb_timeout);
        if (knob_len > 0 && parse_knobs_response(knob_resp, (size_t)knob_len, knobs) == 0) {
            *knobs_valid = 1;
            LOG_DEBUG("KNOBPRE? successful — warn1=%d, warn2=%d",
                      knobs->warn1, knobs->warn2);
        } else {
            LOG_DEBUG("KNOBPRE? query failed or no data");
        }
        libusb_interrupt_transfer(handle, 0x81, flush_buf, 16, &flush_transferred, usb_timeout);
    }
}

int main(int argc, char *argv[])
{
    config_t cfg;
    config_init_from_env(&cfg);

    int opt;
    while ((opt = getopt(argc, argv, "dvoh")) != -1) {
        switch (opt) {
            case 'd': log_level = LOG_LEVEL_DEBUG; break;
            case 'v': cfg.print_voc_only = 1; break;
            case 'o': cfg.one_read = 1; break;
            case 'h': /* fallthrough */
            default:  help();
        }
    }

    char avail_topic[256];
    init_mqtt(&cfg, avail_topic, sizeof(avail_topic));
    int expire_after = cfg.poll_interval * 3;

    devh = init_usb(0x03eb, 0x2013, cfg.usb_timeout);

    device_flags_t dev_flags;
    int flags_valid;
    device_knobs_t dev_knobs;
    int knobs_valid;
    query_device_info(devh, cfg.usb_timeout,
                      &dev_flags, &flags_valid, &dev_knobs, &knobs_valid);

    int ret;
    int transferred;
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;
    int vendor = 0x03eb;
    int product = 0x2013;
    unsigned short iresult = 0;
    unsigned short voc = 0;
    unsigned short rh_raw = 0;
    unsigned int r_s = 0;

    // Publish Home Assistant MQTT auto-discovery configuration
    const char *origin_block =
        "\"origin\":{\"name\":\"airsensor-mqtt\","
        "\"sw_version\":\"" APP_VERSION "\","
        "\"support_url\":\"https://github.com/olcond/airsensor-mqtt\"}";

    const char *manufacturer = device_manufacturer[0] ? device_manufacturer : "AppliedSensor";
    const char *model = device_product[0] ? device_product : "USB VOC Sensor";

    char device_block[512];
    if (device_serial[0]) {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"%s\","
                 "\"manufacturer\":\"%s\","
                 "\"serial_number\":\"%s\","
                 "\"sw_version\":\"%s\"}",
                 cfg.clientid, cfg.ha_device_name, model, manufacturer, device_serial, (device_firmware[0] ? device_firmware : "unknown"));
    } else {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"%s\","
                 "\"manufacturer\":\"%s\"}",
                 cfg.clientid, cfg.ha_device_name, model, manufacturer);
    }

    // VOC discovery
    discovery_entity_t voc_ent = {
        .suffix = NULL, .name = "VOC",
        .value_tpl = "{{ value_json.voc }}", .unit = "ppm",
        .device_class = "volatile_organic_compounds_parts",
        .state_class = "measurement", .entity_cat = NULL, .icon = NULL,
        .precision = 0, .expire_after = expire_after,
        .state_topic = cfg.topic, .avail_topic = avail_topic
    };
    publish_discovery(client, &cfg, device_block, origin_block, &voc_ent);

    // Heating resistance (r_h) discovery
    discovery_entity_t rh_ent = {
        .suffix = "_rh", .name = "Heating Resistance",
        .value_tpl = "{{ value_json.r_h }}", .unit = "Ω",
        .device_class = NULL, .state_class = "measurement",
        .entity_cat = NULL, .icon = "mdi:resistor",
        .precision = 2, .expire_after = expire_after,
        .state_topic = cfg.topic, .avail_topic = avail_topic
    };
    publish_discovery(client, &cfg, device_block, origin_block, &rh_ent);

    // Sensor resistance (r_s) discovery
    discovery_entity_t rs_ent = {
        .suffix = "_rs", .name = "Sensor Resistance",
        .value_tpl = "{{ value_json.r_s }}", .unit = "Ω",
        .device_class = NULL, .state_class = "measurement",
        .entity_cat = NULL, .icon = "mdi:resistor",
        .precision = 0, .expire_after = expire_after,
        .state_topic = cfg.topic, .avail_topic = avail_topic
    };
    publish_discovery(client, &cfg, device_block, origin_block, &rs_ent);

    // Diagnostic topic for flags/knobs (published once at startup)
    char diag_topic[256];
    snprintf(diag_topic, sizeof(diag_topic), "%s/diag", cfg.topic);

    // Warmup discovery (diagnostic)
    if (flags_valid) {
        discovery_entity_t warmup_ent = {
            .suffix = "_warmup", .name = "Warmup",
            .value_tpl = "{{ value_json.warmup }}", .unit = "min",
            .device_class = NULL, .state_class = NULL,
            .entity_cat = "diagnostic", .icon = NULL,
            .precision = -1, .expire_after = -1,
            .state_topic = diag_topic, .avail_topic = avail_topic
        };
        publish_discovery(client, &cfg, device_block, origin_block, &warmup_ent);
    }

    // Warn threshold discovery (diagnostic)
    if (knobs_valid) {
        discovery_entity_t warn1_ent = {
            .suffix = "_warn1", .name = "Warn Threshold 1",
            .value_tpl = "{{ value_json.warn1 }}", .unit = "ppm",
            .device_class = NULL, .state_class = NULL,
            .entity_cat = "diagnostic", .icon = NULL,
            .precision = -1, .expire_after = -1,
            .state_topic = diag_topic, .avail_topic = avail_topic
        };
        publish_discovery(client, &cfg, device_block, origin_block, &warn1_ent);

        discovery_entity_t warn2_ent = {
            .suffix = "_warn2", .name = "Warn Threshold 2",
            .value_tpl = "{{ value_json.warn2 }}", .unit = "ppm",
            .device_class = NULL, .state_class = NULL,
            .entity_cat = "diagnostic", .icon = NULL,
            .precision = -1, .expire_after = -1,
            .state_topic = diag_topic, .avail_topic = avail_topic
        };
        publish_discovery(client, &cfg, device_block, origin_block, &warn2_ent);
    }

    // Publish diagnostic data once at startup
    if (flags_valid || knobs_valid) {
        char diag_payload[512];
        snprintf(diag_payload, sizeof(diag_payload),
                 "{\"warmup\":%u,\"burn_in\":%u,\"warn1\":%u,\"warn2\":%u}",
                 dev_flags.warmup, dev_flags.burn_in, dev_knobs.warn1, dev_knobs.warn2);
        pubmsg.payload = diag_payload;
        pubmsg.payloadlen = (int)strlen(diag_payload);
        pubmsg.qos = QOS;
        pubmsg.retained = 1;
        MQTTClient_publishMessage(client, diag_topic, &pubmsg, &token);
        MQTTClient_waitForCompletion(client, token, TIMEOUT);
        LOG_DEBUG("Diagnostic data published");
    }

    int fail_count = 0;

    rc = MQTTCLIENT_SUCCESS;
    while(rc==MQTTCLIENT_SUCCESS) {

        if (shutdown_requested)
            break;

        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);

        // Build poll command with sequence number (FHEM protocol)
        LOG_DEBUG("Write data to device");

        char poll_cmd[16];
        build_poll_command(poll_seq, poll_cmd);
        poll_seq = next_poll_seq(poll_seq);
        ret = libusb_interrupt_transfer(devh, 0x02, (unsigned char*)poll_cmd, 16, &transferred, cfg.usb_timeout);

        LOG_DEBUG("Return code from USB write: %d (transferred: %d)", ret, transferred);

        if (ret != 0 || transferred != 16) {
            fail_count++;
            LOG_DEBUG("Write failed, fail_count: %d", fail_count);
            if (fail_count >= cfg.max_retries) {
                LOG_ERROR("Max retries reached, reconnecting USB");
                libusb_release_interface(devh, 0);
                libusb_close(devh);
                fail_count = 0;
                // Re-open device
                devh = libusb_open_device_with_vid_pid(NULL, (uint16_t)vendor, (uint16_t)product);
                if (!devh) {
                    LOG_ERROR("Device not found on reconnect");
                    MQTTClient_disconnect(client, 10000);
                    MQTTClient_destroy(&client);
                    libusb_exit(NULL);
                    exit(1);
                }
                if (libusb_kernel_driver_active(devh, 0) == 1)
                    libusb_detach_kernel_driver(devh, 0);
                ret = libusb_claim_interface(devh, 0);
                if (ret != 0) {
                    LOG_ERROR("Claim failed on reconnect: %d", ret);
                    libusb_close(devh);
                    MQTTClient_disconnect(client, 10000);
                    MQTTClient_destroy(&client);
                    libusb_exit(NULL);
                    exit(1);
                }
                LOG_INFO("USB reconnect successful");
                unsigned char flush_buf[16];
                int flush_transferred;
                libusb_interrupt_transfer(devh, 0x81, flush_buf, 16, &flush_transferred, cfg.usb_timeout);
            }
            sleep(cfg.poll_interval);
            continue;
        }

        LOG_DEBUG("Read USB (Chunk 1 of 3)");

        unsigned char fullbuf[48];
        memset(fullbuf, 0, 48);

        ret = libusb_interrupt_transfer(devh, 0x81, fullbuf, 16, &transferred, cfg.usb_timeout);
        LOG_DEBUG("Return code from USB read 1: %d (transferred: %d)", ret, transferred);

        if (ret == 0 && transferred == 0) {
            sleep(1);
            ret = libusb_interrupt_transfer(devh, 0x81, fullbuf, 16, &transferred, cfg.usb_timeout);
            LOG_DEBUG("Return code from USB read 1 (retry): %d (transferred: %d)", ret, transferred);
        }

        if (ret == 0 && transferred == 16) {
            int trans2;
            int ret2 = libusb_interrupt_transfer(devh, 0x81, fullbuf + 16, 16, &trans2, cfg.usb_timeout);
            LOG_DEBUG("Return code from USB read 2: %d (transferred: %d)", ret2, trans2);
            if (ret2 == 0 && trans2 == 16) {
                int trans3;
                int ret3 = libusb_interrupt_transfer(devh, 0x81, fullbuf + 32, 16, &trans3, cfg.usb_timeout);
                LOG_DEBUG("Return code from USB read 3: %d (transferred: %d)", ret3, trans3);
            }
        }

        if (ret != 0)
        {
            fail_count++;
            LOG_DEBUG("Read failed, fail_count: %d", fail_count);
            if (cfg.print_voc_only == 1) {
                printf("0\n");
            } else {
                LOG_ERROR("Invalid result code: %d", ret);
            }
            sleep(cfg.poll_interval);
            continue;
        }

        // Successful read — reset fail counter
        fail_count = 0;

        memcpy(&iresult, fullbuf+2, 2);
        voc = le16toh(iresult);

        unsigned short debug_val = 0;
        memcpy(&debug_val, fullbuf+4, 2);
        debug_val = le16toh(debug_val);

        unsigned short pwm_val = 0;
        memcpy(&pwm_val, fullbuf+6, 2);
        pwm_val = le16toh(pwm_val);

        memcpy(&rh_raw, fullbuf + 8, 2);
        rh_raw = le16toh(rh_raw);

        r_s = (unsigned int)fullbuf[12]
            | ((unsigned int)fullbuf[13] << 8)
            | ((unsigned int)fullbuf[14] << 16);

        LOG_DEBUG("r_h raw: %d", rh_raw);
        LOG_DEBUG("r_s: %u", r_s);

        sleep(1);

        LOG_DEBUG("Read USB [flush]");

        {
            unsigned char flush_buf[16];
            int flush_transferred;
            ret = libusb_interrupt_transfer(devh, 0x81, flush_buf, 16, &flush_transferred, cfg.usb_timeout);
        }

        LOG_DEBUG("Return code from USB flush read: %d", ret);

        if (voc_in_range(voc)) {
            if (cfg.print_voc_only == 1) {
                printf("%d\n", voc);
            } else {
                printf("%04d-%02d-%02d %02d:%02d:%02d, ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                printf("VOC: %d, RESULT: OK\n", voc);
            }

            char json_payload[512];
            snprintf(json_payload, sizeof(json_payload),
                "{\"voc\":%u,\"r_h\":%.2f,\"r_s\":%u,\"debug\":%u,\"pwm\":%u}",
                voc, rh_raw / 100.0, r_s, debug_val, pwm_val);

            pubmsg.payload = json_payload;
            pubmsg.payloadlen = (int)strlen(json_payload);
            pubmsg.qos = QOS;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(client, cfg.topic, &pubmsg, &token);
            LOG_DEBUG("Publishing %s on topic %s", (char*)pubmsg.payload, cfg.topic);
            rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);
            LOG_DEBUG("Message with delivery token %d delivered", token);

        } else {
            if (cfg.print_voc_only == 1) {
                printf("0\n");
            } else {
                printf("%04d-%02d-%02d %02d:%02d:%02d, ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
                printf("VOC: %d, RESULT: Error value out of range\n", voc);
            }
        }

        if (cfg.one_read == 1)
            break;

        sleep(cfg.poll_interval);

        if (shutdown_requested)
            break;

    }

    libusb_release_interface(devh, 0);
    libusb_close(devh);
    libusb_exit(NULL);
    {
        MQTTClient_message off_msg = MQTTClient_message_initializer;
        MQTTClient_deliveryToken off_tk;
        off_msg.payload = "offline";
        off_msg.payloadlen = 7;
        off_msg.qos = QOS;
        off_msg.retained = 1;
        MQTTClient_publishMessage(client, g_avail_topic, &off_msg, &off_tk);
        MQTTClient_waitForCompletion(client, off_tk, TIMEOUT);
    }
    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);
    return 0;

}

/*

	airsensor.c

	Original source: Rodric Yates http://code.google.com/p/airsensor-linux-usb/
	Modified from source: Ap15e (MiOS) http://wiki.micasaverde.com/index.php/CO2_Sensor
	Modified by Sebastian Sjoholm, sebastian.sjoholm@gmail.com

	This version created by Veit Olschinski, volschin@googlemail.com

	requirement:

	libusb libpaho-mqtt3c libpthread

	compile:

	gcc -o airsensor airsensor.c -lusb -lpaho-mqtt3c -lpthread

*/

#define _GNU_SOURCE

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <usb.h>
#include <endian.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include "MQTTClient.h"
#include "airsensor.h"

#define QOS         1
#define TIMEOUT     10000L

int log_level = LOG_LEVEL_INFO;

MQTTClient client;
struct usb_dev_handle *devh;
char device_serial[20] = "";
char device_firmware[20] = "";
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

struct usb_device* find_device(int vendor, int product) {
	struct usb_bus *bus;

	for (bus = usb_get_busses(); bus; bus = bus->next) {
		struct usb_device *dev;

		for (dev = bus->devices; dev; dev = dev->next) {
			if (dev->descriptor.idVendor == vendor
				&& dev->descriptor.idProduct == product)
			return dev;
		}
	}
	return NULL;
}


/*
 * Send a data query command and read N response chunks.
 * Returns total bytes read, or -1 on write failure.
 */
static int query_device_data(struct usb_dev_handle *handle, const char *reqstr,
                             unsigned char *resp_buf, int num_chunks, int timeout_ms) {
    static unsigned short data_seq = 1;
    char cmd[16];
    build_data_command(data_seq, reqstr, cmd);
    data_seq = (data_seq < 0xFFFF) ? (unsigned short)(data_seq + 1) : 1;

    int ret = usb_interrupt_write(handle, 0x00000002, cmd, 16, timeout_ms);
    if (ret != 16) return -1;

    int total = 0;
    for (int i = 0; i < num_chunks; i++) {
        ret = usb_interrupt_read(handle, 0x00000081, (char*)resp_buf + total, 16, timeout_ms);
        if (ret > 0) total += ret;
        else break;
    }
    return total;
}

/*
 * Send a Type 1 command (*IDN?) to query device identification.
 * Reads the response into resp_buf (null-terminated string).
 * Returns 0 on success, -1 on failure.
 */
int query_device_id(struct usb_dev_handle *handle, char *resp_buf, size_t resp_size, int timeout_ms) {
    static unsigned short idn_seq = 1;
    char cmd[17];
    snprintf(cmd, sizeof(cmd), "@%04X*IDN?\n@@@@@", idn_seq);
    idn_seq = (idn_seq < 0xFFFF) ? (unsigned short)(idn_seq + 1) : 1;
    int ret = usb_interrupt_write(handle, 0x00000002, cmd, 16, timeout_ms);
    if (ret < 0) return -1;

    size_t total = 0;
    char chunk[16];
    int attempts = 0;
    while (total < resp_size - 1 && attempts < 10) {
        ret = usb_interrupt_read(handle, 0x00000081, chunk, 16, timeout_ms);
        if (ret <= 0) break;
        int start = (attempts == 0) ? 1 : 0;
        for (int i = start; i < ret && total < resp_size - 1; i++) {
            if (chunk[i] == '@') continue;
            resp_buf[total++] = chunk[i];
        }
        attempts++;
    }
    resp_buf[total] = '\0';
    return (total > 0) ? 0 : -1;
}

int main(int argc, char *argv[])
{
    config_t cfg;
    config_init_from_env(&cfg);
    char address[256];
    snprintf(address, sizeof(address), "tcp://%s:%s", cfg.broker, cfg.port);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    char avail_topic[256];
    snprintf(avail_topic, sizeof(avail_topic), "%s/availability", cfg.topic);
    snprintf(g_avail_topic, sizeof(g_avail_topic), "%s", avail_topic);
    int expire_after = cfg.poll_interval * 3;

    MQTTClient_create(&client, address, cfg.clientid,
        MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 70;
    conn_opts.cleansession = 1;
    conn_opts.username = cfg.mqtt_username;
    conn_opts.password = cfg.mqtt_password;

    will_opts.topicName = avail_topic;
    will_opts.message = "offline";
    will_opts.qos = QOS;
    will_opts.retained = 1;
    conn_opts.will = &will_opts;

    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        LOG_ERROR("Failed to connect, return code %d", rc);
        exit(EXIT_FAILURE);
    }

    // Publish online availability (retained)
    pubmsg.payload = "online";
    pubmsg.payloadlen = 6;
    pubmsg.qos = QOS;
    pubmsg.retained = 1;
    MQTTClient_publishMessage(client, avail_topic, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, TIMEOUT);

	int ret, vendor, product, counter;
	struct usb_device *dev;
	char buf[1000];

	vendor = 0x03eb;
	product = 0x2013;
	dev = NULL;

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

	LOG_DEBUG("Init USB");

	usb_init();

	counter = 0;

	do {

		usb_set_debug(0);
		usb_find_busses();
		usb_find_devices();
		dev = find_device(vendor, product);

		if (dev != NULL)
			break;

		LOG_DEBUG("No device found, wait 10sec...");

		sleep(11);
		++counter;

		if (counter == 10) {
			LOG_ERROR("Device not found");
			MQTTClient_disconnect(client, 10000);
			MQTTClient_destroy(&client);
			exit(1);
		}

	}  while (1);

	assert(dev);

	LOG_DEBUG("USB device found");

	devh = usb_open(dev);
	if (!devh) {
		LOG_ERROR("Failed to open USB device");
		MQTTClient_disconnect(client, 10000);
		MQTTClient_destroy(&client);
		exit(1);
	}

	if (dev->descriptor.iManufacturer) {
		usb_get_string_simple(devh, dev->descriptor.iManufacturer,
		                      device_manufacturer, sizeof(device_manufacturer));
	}
	if (dev->descriptor.iProduct) {
		usb_get_string_simple(devh, dev->descriptor.iProduct,
		                      device_product, sizeof(device_product));
	}

	if (device_manufacturer[0])
		LOG_DEBUG("Manufacturer: %s", device_manufacturer);
	if (device_product[0])
		LOG_DEBUG("Product: %s", device_product);

	signal(SIGTERM, release_usb_device);
	signal(SIGINT,  release_usb_device);

	ret = usb_get_driver_np(devh, 0, buf, sizeof(buf));
	if (ret == 0) {
		ret = usb_detach_kernel_driver_np(devh, 0);
	}

	ret = usb_claim_interface(devh, 0);
	if (ret != 0) {
		LOG_ERROR("Claim failed with error: %d", ret);
		usb_close(devh);
		MQTTClient_disconnect(client, 10000);
		MQTTClient_destroy(&client);
		exit(1);
	}

	unsigned short iresult=0;
	unsigned short voc=0;
	unsigned short rh_raw = 0;
	unsigned int r_s = 0;

	LOG_DEBUG("Read any remaining data from USB");

	ret = usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);

	LOG_DEBUG("Return code from USB read: %d", ret);

	/* Query device identification (*IDN?) for serial and firmware */
	{
		char idn_response[256];
		if (query_device_id(devh, idn_response, sizeof(idn_response), cfg.usb_timeout) == 0) {

			parse_serial_from_idn_response(idn_response,
			                               device_serial,
			                               sizeof(device_serial));
			parse_firmware_from_idn_response(idn_response,
			                                 device_firmware,
			                                 sizeof(device_firmware));

			// Robust fallback if serial was not found via S/N: marker
			if (device_serial[0] == '\0') {
				size_t dest_idx = 0;
				size_t j = 0;
				while (idn_response[j] && dest_idx < sizeof(device_serial) - 1) {
					char c = idn_response[j];
					if (!is_idn_delim(c)) {
						device_serial[dest_idx++] = c;
					}
					j++;
				}
				device_serial[dest_idx] = '\0';
			}
			if (device_serial[0])
				printf("Device serial: %s\n", device_serial);
			if (device_firmware[0] == '\0')
				snprintf(device_firmware, sizeof(device_firmware),
				         "1.12p5 $Revision: 346");
			printf("Device firmware: %s\n", device_firmware);
			LOG_DEBUG("*IDN? query successful");
		} else {
			LOG_DEBUG("*IDN? query failed, continuing without device info");
		}
		/* Flush any remaining data */
		usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);
	}

    /* Query device flags (FLAGGET?) for warmup/burn-in status */
    device_flags_t dev_flags = {0};
    int flags_valid = 0;
    {
        unsigned char flag_resp[48];
        int flag_len = query_device_data(devh, "FLAGGET?", flag_resp, 3, cfg.usb_timeout);
        if (flag_len > 0 && parse_flags_response(flag_resp, (size_t)flag_len, &dev_flags) == 0) {
            flags_valid = 1;
            if (dev_flags.warmup > 0)
                printf("Warmup: %d min remaining\n", dev_flags.warmup);
            if (dev_flags.burn_in > 0)
                printf("Burn-in: %d min remaining\n", dev_flags.burn_in);
            LOG_DEBUG("FLAGGET? query successful");
            LOG_DEBUG("Warmup: %d", dev_flags.warmup);
            LOG_DEBUG("Burn-in: %d", dev_flags.burn_in);
            LOG_DEBUG("Logging: %d", dev_flags.logging);
        } else {
            LOG_DEBUG("FLAGGET? query failed");
        }
        usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);
    }

    /* Query device knobs (KNOBPRE?) for warn thresholds */
    device_knobs_t dev_knobs = {0};
    int knobs_valid = 0;
    {
        unsigned char knob_resp[256];
        int knob_len = query_device_data(devh, "KNOBPRE?", knob_resp, 16, cfg.usb_timeout);
        if (knob_len > 0 && parse_knobs_response(knob_resp, (size_t)knob_len, &dev_knobs) == 0) {
            knobs_valid = 1;
            LOG_DEBUG("KNOBPRE? query successful");
            LOG_DEBUG("warn1 threshold: %d", dev_knobs.warn1);
            LOG_DEBUG("warn2 threshold: %d", dev_knobs.warn2);
        } else {
            LOG_DEBUG("KNOBPRE? query failed or no data");
        }
        usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);
    }

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

    MQTTClient_message disc_msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken disc_token;
    disc_msg.qos = QOS;
    disc_msg.retained = 1;

    // VOC discovery
    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/sensor/%s/config", cfg.ha_prefix, cfg.clientid);
    char discovery_payload[1536];
    snprintf(discovery_payload, sizeof(discovery_payload),
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
             "%s,%s}",
             cfg.ha_device_name, cfg.clientid, cfg.topic, cfg.clientid,
             avail_topic, expire_after, device_block, origin_block);
    disc_msg.payload = discovery_payload;
    disc_msg.payloadlen = (int)strlen(discovery_payload);
    MQTTClient_publishMessage(client, discovery_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

    // Heating resistance (r_h) discovery
    char rh_disc_topic[256];
    snprintf(rh_disc_topic, sizeof(rh_disc_topic),
             "%s/sensor/%s_rh/config", cfg.ha_prefix, cfg.clientid);
    char rh_disc_payload[1536];
    snprintf(rh_disc_payload, sizeof(rh_disc_payload),
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
             "%s,%s}",
             cfg.ha_device_name, cfg.clientid, cfg.topic, cfg.clientid,
             avail_topic, expire_after, device_block, origin_block);
    disc_msg.payload = rh_disc_payload;
    disc_msg.payloadlen = (int)strlen(rh_disc_payload);
    MQTTClient_publishMessage(client, rh_disc_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

    // Sensor resistance (r_s) discovery
    char rs_disc_topic[256];
    snprintf(rs_disc_topic, sizeof(rs_disc_topic),
             "%s/sensor/%s_rs/config", cfg.ha_prefix, cfg.clientid);
    char rs_disc_payload[1536];
    snprintf(rs_disc_payload, sizeof(rs_disc_payload),
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
             "%s,%s}",
             cfg.ha_device_name, cfg.clientid, cfg.topic, cfg.clientid,
             avail_topic, expire_after, device_block, origin_block);
    disc_msg.payload = rs_disc_payload;
    disc_msg.payloadlen = (int)strlen(rs_disc_payload);
    MQTTClient_publishMessage(client, rs_disc_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

    // Diagnostic topic for flags/knobs (published once at startup)
    char diag_topic[256];
    snprintf(diag_topic, sizeof(diag_topic), "%s/diag", cfg.topic);

    // Warmup discovery (diagnostic)
    if (flags_valid) {
        char warmup_disc_topic[256];
        snprintf(warmup_disc_topic, sizeof(warmup_disc_topic),
                 "%s/sensor/%s_warmup/config", cfg.ha_prefix, cfg.clientid);
        char warmup_disc_payload[1536];
        snprintf(warmup_disc_payload, sizeof(warmup_disc_payload),
                 "{\"name\":\"%s Warmup\","
                 "\"object_id\":\"%s_warmup\","
                 "\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.warmup }}\","
                 "\"unit_of_measurement\":\"min\","
                 "\"entity_category\":\"diagnostic\","
                 "\"unique_id\":\"%s_warmup\","
                 "\"availability_topic\":\"%s\","
                 "%s,%s}",
                 cfg.ha_device_name, cfg.clientid, diag_topic, cfg.clientid,
                 avail_topic, device_block, origin_block);
        disc_msg.payload = warmup_disc_payload;
        disc_msg.payloadlen = (int)strlen(warmup_disc_payload);
        MQTTClient_publishMessage(client, warmup_disc_topic, &disc_msg, &disc_token);
        MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);
    }

    // Warn threshold discovery (diagnostic)
    if (knobs_valid) {
        char warn1_disc_topic[256];
        snprintf(warn1_disc_topic, sizeof(warn1_disc_topic),
                 "%s/sensor/%s_warn1/config", cfg.ha_prefix, cfg.clientid);
        char warn1_disc_payload[1536];
        snprintf(warn1_disc_payload, sizeof(warn1_disc_payload),
                 "{\"name\":\"%s Warn Threshold 1\","
                 "\"object_id\":\"%s_warn1\","
                 "\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.warn1 }}\","
                 "\"unit_of_measurement\":\"ppm\","
                 "\"entity_category\":\"diagnostic\","
                 "\"unique_id\":\"%s_warn1\","
                 "\"availability_topic\":\"%s\","
                 "%s,%s}",
                 cfg.ha_device_name, cfg.clientid, diag_topic, cfg.clientid,
                 avail_topic, device_block, origin_block);
        disc_msg.payload = warn1_disc_payload;
        disc_msg.payloadlen = (int)strlen(warn1_disc_payload);
        MQTTClient_publishMessage(client, warn1_disc_topic, &disc_msg, &disc_token);
        MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

        char warn2_disc_topic[256];
        snprintf(warn2_disc_topic, sizeof(warn2_disc_topic),
                 "%s/sensor/%s_warn2/config", cfg.ha_prefix, cfg.clientid);
        char warn2_disc_payload[1536];
        snprintf(warn2_disc_payload, sizeof(warn2_disc_payload),
                 "{\"name\":\"%s Warn Threshold 2\","
                 "\"object_id\":\"%s_warn2\","
                 "\"state_topic\":\"%s\","
                 "\"value_template\":\"{{ value_json.warn2 }}\","
                 "\"unit_of_measurement\":\"ppm\","
                 "\"entity_category\":\"diagnostic\","
                 "\"unique_id\":\"%s_warn2\","
                 "\"availability_topic\":\"%s\","
                 "%s,%s}",
                 cfg.ha_device_name, cfg.clientid, diag_topic, cfg.clientid,
                 avail_topic, device_block, origin_block);
        disc_msg.payload = warn2_disc_payload;
        disc_msg.payloadlen = (int)strlen(warn2_disc_payload);
        MQTTClient_publishMessage(client, warn2_disc_topic, &disc_msg, &disc_token);
        MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);
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
		ret = usb_interrupt_write(devh, 0x00000002, poll_cmd, 16, cfg.usb_timeout);

		LOG_DEBUG("Return code from USB write: %d", ret);

		if (ret != 16) {
			fail_count++;
			LOG_DEBUG("Write failed, fail_count: %d", fail_count);
			if (fail_count >= cfg.max_retries) {
				LOG_ERROR("Max retries reached, reconnecting USB");
				usb_release_interface(devh, 0);
				usb_close(devh);
				fail_count = 0;
				// Re-open device
				usb_find_busses();
				usb_find_devices();
				dev = find_device(vendor, product);
				if (!dev) {
					LOG_ERROR("Device not found on reconnect");
					MQTTClient_disconnect(client, 10000);
					MQTTClient_destroy(&client);
					exit(1);
				}
				devh = usb_open(dev);
				if (!devh) {
					LOG_ERROR("Failed to reopen USB device");
					MQTTClient_disconnect(client, 10000);
					MQTTClient_destroy(&client);
					exit(1);
				}
				ret = usb_get_driver_np(devh, 0, buf, sizeof(buf));
				if (ret == 0)
					usb_detach_kernel_driver_np(devh, 0);
				ret = usb_claim_interface(devh, 0);
				if (ret != 0) {
					LOG_ERROR("Claim failed on reconnect: %d", ret);
					usb_close(devh);
					MQTTClient_disconnect(client, 10000);
					MQTTClient_destroy(&client);
					exit(1);
				}
				LOG_INFO("USB reconnect successful");
				usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);
			}
			sleep(cfg.poll_interval);
			continue;
		}

		LOG_DEBUG("Read USB (Chunk 1 of 3)");

		unsigned char fullbuf[48];
		memset(fullbuf, 0, 48);

		ret = usb_interrupt_read(devh, 0x00000081, (char*)fullbuf, 16, cfg.usb_timeout);
		LOG_DEBUG("Return code from USB read 1: %d", ret);

		if (ret == 0) {
			sleep(1);
			ret = usb_interrupt_read(devh, 0x00000081, (char*)fullbuf, 16, cfg.usb_timeout);
			LOG_DEBUG("Return code from USB read 1 (retry): %d", ret);
		}

		if (ret == 16) {
			int ret2 = usb_interrupt_read(devh, 0x00000081, (char*)fullbuf + 16, 16, cfg.usb_timeout);
			LOG_DEBUG("Return code from USB read 2: %d", ret2);
			if (ret2 == 16) {
				int ret3 = usb_interrupt_read(devh, 0x00000081, (char*)fullbuf + 32, 16, cfg.usb_timeout);
				LOG_DEBUG("Return code from USB read 3: %d", ret3);
			}
		}

		if ( !((ret == 0) || (ret == 16)))
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

		ret = usb_interrupt_read(devh, 0x00000081, buf, 16, cfg.usb_timeout);

		LOG_DEBUG("Return code from USB read: %d", ret);

		if ( voc >= 450 && voc <= 15001) {
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
            printf("Waiting for up to %d seconds for publication of %s\non topic %s for client with ClientID: %s\n",
               (int)(TIMEOUT/1000), (char*)pubmsg.payload, cfg.topic, cfg.clientid);
            rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);
            printf("Message with delivery token %d delivered\n", token);

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

	usb_release_interface(devh, 0);
	usb_close(devh);
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

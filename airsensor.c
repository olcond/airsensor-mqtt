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

#define QOS         1
#define TIMEOUT     10000L

MQTTClient client;
struct usb_dev_handle *devh;
char device_serial[20] = "";
char device_firmware[20] = "";

void help() {

	printf("AirSensor [options]\n");
	printf("Options:\n");
	printf("-d = debug printout\n");
	printf("-v = Print VOC value only, nothing returns if value out of range (450-2000)\n");
	printf("-o = One value and then exit\n");
	printf("-h = Help, this printout\n");
	exit(0);

}

void printout(char *str, int value) {

	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	printf("%04d-%02d-%02d %02d:%02d:%02d, ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (value == 0) {
		printf("%s\n", str);
	} else {
		printf("%s %d\n", str, value);
	}
}

void release_usb_device(int dummy) {
	int ret;
	ret = usb_release_interface(devh, 0);
	usb_close(devh);
    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);
	exit(ret);
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
 * Return non-zero if c is a field delimiter in an *IDN? response.
 * Delimiters: space, newline, carriage-return, semicolon, at-sign.
 */
static int is_idn_delim(char c) {
    return (c == ' ' || c == '\n' || c == '\r' || c == ';' || c == '@');
}

/*
 * Parse the serial number from an *IDN? response string.
 * Looks for "S/N:" and extracts characters until the next delimiter or
 * end of string.  Writes a null-terminated result into out/out_size.
 * Returns 0 on success, -1 if the marker is not found.
 */
static int parse_serial_from_idn_response(const char *response,
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
static int parse_firmware_from_idn_response(const char *response,
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

/*
 * Send a Type 1 command (*IDN?) to query device identification.
 * Reads the response into resp_buf (null-terminated string).
 * Returns 0 on success, -1 on failure.
 */
int query_device_id(struct usb_dev_handle *handle, char *resp_buf, size_t resp_size) {
    static unsigned short idn_seq = 1;
    char cmd[17];
    snprintf(cmd, sizeof(cmd), "@%04X*IDN?\n@@@@@", idn_seq);
    idn_seq = (idn_seq < 0xFFFF) ? (unsigned short)(idn_seq + 1) : 1;
    int ret = usb_interrupt_write(handle, 0x00000002, cmd, 16, 1000);
    if (ret < 0) return -1;

    size_t total = 0;
    char chunk[16];
    int attempts = 0;
    while (total < resp_size - 1 && attempts < 10) {
        ret = usb_interrupt_read(handle, 0x00000081, chunk, 16, 1000);
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
    const char *brokername = getenv("MQTT_BROKERNAME");
    if (!brokername) brokername = "127.0.0.1";
    const char *portnumber = getenv("MQTT_PORT");
    if (!portnumber) portnumber = "1883";
    const char *clientid = getenv("MQTT_CLIENTID");
    if (!clientid) clientid = "airsensor";
    const char *topicname = getenv("MQTT_TOPIC");
    if (!topicname) topicname = "home/CO2/voc";
    const char *ha_prefix = getenv("HA_DISCOVERY_PREFIX");
    if (!ha_prefix) ha_prefix = "homeassistant";
    const char *ha_device_name = getenv("HA_DEVICE_NAME");
    if (!ha_device_name) ha_device_name = "Air Sensor";
    const char *topic_humidity = getenv("MQTT_TOPIC_HUMIDITY");
    if (!topic_humidity) topic_humidity = "home/CO2/humidity";
    const char *topic_resistance = getenv("MQTT_TOPIC_RESISTANCE");
    if (!topic_resistance) topic_resistance = "home/CO2/resistance";
    char address[256];
    snprintf(address, sizeof(address), "tcp://%s:%s", brokername, portnumber);

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;
    MQTTClient_create(&client, address, clientid,
        MQTTCLIENT_PERSISTENCE_NONE, NULL);
    conn_opts.keepAliveInterval = 70;
    conn_opts.cleansession = 1;
    conn_opts.username = getenv("MQTT_USERNAME");
    conn_opts.password = getenv("MQTT_PASSWORD");
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS)
    {
        printf("Failed to connect, return code %d\n", rc);
        exit(EXIT_FAILURE);
    }

	int ret, vendor, product, debug, counter, one_read;
	int print_voc_only;
	struct usb_device *dev;
	char buf[1000];

	debug = 0;
	print_voc_only = 0;
	one_read = 0;

	vendor = 0x03eb;
	product = 0x2013;
	dev = NULL;

	while ((argc > 1) && (argv[1][0] == '-'))
	{
		switch (argv[1][1])
		{
			case 'd':
				debug = 1;
				break;

			case 'v':
				print_voc_only = 1;
				break;

			case 'o':
				one_read = 1;
				break;

			case 'h':
				help();

		}

		++argv;
		--argc;
	}

	if (debug == 1) {
		printout("DEBUG: Active", 0);
	}

	if (debug == 1) {
		printout("DEBUG: Init USB", 0);
	}

	usb_init();

	counter = 0;

	do {

		usb_set_debug(0);
		usb_find_busses();
		usb_find_devices();
		dev = find_device(vendor, product);

		if (dev != NULL)
			break;

		if (debug == 1)
			printout("DEBUG: No device found, wait 10sec...", 0);

		sleep(11);
		++counter;

		if (counter == 10) {
			printout("Error: Device not found", 0);
			MQTTClient_disconnect(client, 10000);
			MQTTClient_destroy(&client);
			exit(1);
		}

	}  while (1);

	assert(dev);

	if (debug == 1)
		printout("DEBUG: USB device found", 0);

	devh = usb_open(dev);
	if (!devh) {
		printout("Error: Failed to open USB device", 0);
		MQTTClient_disconnect(client, 10000);
		MQTTClient_destroy(&client);
		exit(1);
	}

	signal(SIGTERM, release_usb_device);
	signal(SIGINT,  release_usb_device);

	ret = usb_get_driver_np(devh, 0, buf, sizeof(buf));
	if (ret == 0) {
		ret = usb_detach_kernel_driver_np(devh, 0);
	}

	ret = usb_claim_interface(devh, 0);
	if (ret != 0) {
		printout("Error: claim failed with error: ", ret);
		usb_close(devh);
		MQTTClient_disconnect(client, 10000);
		MQTTClient_destroy(&client);
		exit(1);
	}

	unsigned short iresult=0;
	unsigned short voc=0;
	unsigned char humidity_raw = 0;
	unsigned int resistance = 0;

	if (debug == 1)
		printout("DEBUG: Read any remaining data from USB", 0);

	ret = usb_interrupt_read(devh, 0x00000081, buf, 0x0000010, 1000);

	if (debug == 1)
		printout("DEBUG: Return code from USB read: ", ret);

	/* Query device identification (*IDN?) for serial and firmware */
	{
		char idn_response[256];
		if (query_device_id(devh, idn_response, sizeof(idn_response)) == 0) {
			parse_serial_from_idn_response(idn_response,
			                               device_serial,
			                               sizeof(device_serial));
			parse_firmware_from_idn_response(idn_response,
			                                 device_firmware,
			                                 sizeof(device_firmware));
			if (device_serial[0])
				printf("Device serial: %s\n", device_serial);
			if (device_firmware[0])
				printf("Device firmware: %s\n", device_firmware);
			if (debug == 1) {
				printout("DEBUG: *IDN? query successful", 0);
			}
		} else {
			if (debug == 1)
				printout("DEBUG: *IDN? query failed, continuing without device info", 0);
		}
		/* Flush any remaining data */
		usb_interrupt_read(devh, 0x00000081, buf, 0x0000010, 1000);
	}

    // Publish Home Assistant MQTT auto-discovery configuration
    char device_block[512];
    if (device_serial[0] && device_firmware[0]) {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\","
                 "\"serial_number\":\"%s\","
                 "\"sw_version\":\"%s\"}",
                 clientid, ha_device_name, device_serial, device_firmware);
    } else {
        snprintf(device_block, sizeof(device_block),
                 "\"device\":{\"identifiers\":[\"%s\"],"
                 "\"name\":\"%s\","
                 "\"model\":\"USB VOC Sensor\","
                 "\"manufacturer\":\"Atmel\"}",
                 clientid, ha_device_name);
    }

    MQTTClient_message disc_msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken disc_token;
    disc_msg.qos = QOS;
    disc_msg.retained = 1;

    // VOC discovery
    char discovery_topic[256];
    snprintf(discovery_topic, sizeof(discovery_topic),
             "%s/sensor/%s/config", ha_prefix, clientid);
    char discovery_payload[1024];
    snprintf(discovery_payload, sizeof(discovery_payload),
             "{\"name\":\"%s VOC\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"ppm\","
             "\"device_class\":\"volatile_organic_compounds_parts\","
             "\"unique_id\":\"%s_voc\","
             "%s}",
             ha_device_name, topicname, clientid, device_block);
    disc_msg.payload = discovery_payload;
    disc_msg.payloadlen = (int)strlen(discovery_payload);
    MQTTClient_publishMessage(client, discovery_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

    // Humidity discovery
    char humidity_disc_topic[256];
    snprintf(humidity_disc_topic, sizeof(humidity_disc_topic),
             "%s/sensor/%s_humidity/config", ha_prefix, clientid);
    char humidity_disc_payload[1024];
    snprintf(humidity_disc_payload, sizeof(humidity_disc_payload),
             "{\"name\":\"%s Humidity\","
             "\"state_topic\":\"%s\","
             "\"unique_id\":\"%s_humidity\","
             "%s}",
             ha_device_name, topic_humidity, clientid, device_block);
    disc_msg.payload = humidity_disc_payload;
    disc_msg.payloadlen = (int)strlen(humidity_disc_payload);
    MQTTClient_publishMessage(client, humidity_disc_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

    // Resistance discovery
    char resistance_disc_topic[256];
    snprintf(resistance_disc_topic, sizeof(resistance_disc_topic),
             "%s/sensor/%s_resistance/config", ha_prefix, clientid);
    char resistance_disc_payload[1024];
    snprintf(resistance_disc_payload, sizeof(resistance_disc_payload),
             "{\"name\":\"%s Resistance\","
             "\"state_topic\":\"%s\","
             "\"unit_of_measurement\":\"Ohm\","
             "\"unique_id\":\"%s_resistance\","
             "%s}",
             ha_device_name, topic_resistance, clientid, device_block);
    disc_msg.payload = resistance_disc_payload;
    disc_msg.payloadlen = (int)strlen(resistance_disc_payload);
    MQTTClient_publishMessage(client, resistance_disc_topic, &disc_msg, &disc_token);
    MQTTClient_waitForCompletion(client, disc_token, TIMEOUT);

	while(rc==MQTTCLIENT_SUCCESS) {

		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);

		// USB COMMAND TO REQUEST DATA
 		// @h*TR
		if (debug == 1)
			printout("DEBUG: Write data to device", 0);

		memcpy(buf, "\x40\x68\x2a\x54\x52\x0a\x40\x40\x40\x40\x40\x40\x40\x40\x40\x40", 0x0000010);
		ret = usb_interrupt_write(devh, 0x00000002, buf, 0x0000010, 1000);

		if (debug == 1)
			printout("DEBUG: Return code from USB write: ", ret);

		if (debug == 1)
			printout("DEBUG: Read USB", 0);

		ret = usb_interrupt_read(devh, 0x00000081, buf, 0x0000010, 1000);

		if (debug == 1)
			printout("DEBUG: Return code from USB read: ", ret);

		if ( !((ret == 0) || (ret == 16)))
		{
			if (print_voc_only == 1) {
				printf("0\n");
			} else {
				printout("ERROR: Invalid result code: ", ret);
			}
		}

		if (ret == 0) {

			if (debug == 1)
				printout("DEBUG: Read USB", 0);

			sleep(1);
			ret = usb_interrupt_read(devh, 0x00000081, buf, 0x0000010, 1000);

			if (debug == 1)
				printout("DEBUG: Return code from USB read: ", ret);
		}

		memcpy(&iresult,buf+2,2);
		voc = le16toh(iresult);
		humidity_raw = (unsigned char)buf[7];
		memcpy(&resistance, buf + 8, 4);
		resistance = le32toh(resistance);

		if (debug == 1) {
			printout("DEBUG: Humidity raw: ", humidity_raw);
			printout("DEBUG: Resistance: ", resistance);
		}

		sleep(1);

		if (debug == 1) {
			printout("DEBUG: Read USB [flush]", 0);
		}

		ret = usb_interrupt_read(devh, 0x00000081, buf, 0x0000010, 1000);

		if (debug == 1)
			printout("DEBUG: Return code from USB read: ", ret);

 		// According to AppliedSensor specifications the output range is between 450 and 2000
 		// So only printout values between this range

		if ( voc >= 450 && voc <= 15001) {
			if (print_voc_only == 1) {
				printf("%d\n", voc);
			} else {
				printf("%04d-%02d-%02d %02d:%02d:%02d, ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
				printf("VOC: %d, RESULT: OK\n", voc);
			}

            char svoc[6];
            // convert 123 to string [buf]
            snprintf(svoc, sizeof(svoc), "%d", voc);
            pubmsg.payload = svoc;
            pubmsg.payloadlen = strlen(svoc);
            pubmsg.qos = QOS;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(client, topicname, &pubmsg, &token);
            printf("Waiting for up to %d seconds for publication of %s\n"
               "on topic %s for client with ClientID: %s\n",
               (int)(TIMEOUT/1000), pubmsg.payload, topicname, clientid);
            rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);
            printf("Message with delivery token %d delivered\n", token);

            // Publish humidity
            char shum[4];
            snprintf(shum, sizeof(shum), "%u", humidity_raw);
            pubmsg.payload = shum;
            pubmsg.payloadlen = strlen(shum);
            pubmsg.qos = QOS;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(client, topic_humidity, &pubmsg, &token);
            rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);

            // Publish sensor resistance
            char sres[11];
            snprintf(sres, sizeof(sres), "%u", resistance);
            pubmsg.payload = sres;
            pubmsg.payloadlen = strlen(sres);
            pubmsg.qos = QOS;
            pubmsg.retained = 0;
            MQTTClient_publishMessage(client, topic_resistance, &pubmsg, &token);
            rc = MQTTClient_waitForCompletion(client, token, TIMEOUT);

		} else {
			if (print_voc_only == 1) {
				printf("0\n");
			} else {
				printf("%04d-%02d-%02d %02d:%02d:%02d, ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
				printf("VOC: %d, RESULT: Error value out of range\n", voc);
			}
		}

		// If one read, then exit
		if (one_read == 1)
			exit(0);

		// Wait for next request for data
		sleep(30);

	}

}

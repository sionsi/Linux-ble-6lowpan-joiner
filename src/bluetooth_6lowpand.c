/* Copyright (c) 2015, Nordic Semiconductor
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <endian.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include <time.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/mgmt.h"

#include "src/shared/mainloop.h"
#include "src/shared/mgmt.h"
#include "src/shared/util.h"

//#define DEBUG_6LOWPAN

#ifdef DEBUG_6LOWPAN
#define DEBUG_PRINT printf
#else
#define DEBUG_PRINT(...)
#endif

#define DEFAULT_SCANNING_WINDOW   5
#define DEFAULT_SCANNING_INTERVAL 10
#define MAX_SCANNING_WINDOW       30
#define MAX_SCANNING_INTERVAL     300

#define MAX_BLE_CONN              8
#define IPSP_UUID                 0x1820 /* IPSP service UUID */
#define NORDIC_COMPANY_ID         0x0059 /* 16-bit uuid of Nordic Company */

#define EIR_FLAGS                 0x01  /* flags */
#define EIR_UUID16_SOME           0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL            0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME           0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL            0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME          0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL           0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT            0x08  /* shortened local name */
#define EIR_NAME_COMPLETE         0x09  /* complete local name */
#define EIR_TX_POWER              0x0A  /* transmit power level */
#define EIR_DEVICE_ID             0x10  /* device ID */
#define EIR_MANUF_SPECIFIC_DATA   0xFF  /* manufacture specific data */

#define DEVICE_NAME_LEN           30
#define DEVICE_ADDR_LEN           18

#define CONTROLLER_PATH           "/sys/kernel/debug/bluetooth/6lowpan_control"
#define CONFIG_PATH               "/etc/bluetooth/bluetooth_6lowpand.conf"
#define CONFIG_SWP_PATH           "/etc/bluetooth/bluetooth_6lowpand.conf.swp"
#define CONFIG_LINE_MAX           256

#define AUTH_SSID_MAX_LEN         16  /* 16 characters of Service Set Identifier. */
#define AUTH_KEY_LEN              6   /* Currently passkey is used instead of OOB. Key has to have exactly 6 numeric character. */

#define WIFI_CONFIG_PATH          "/etc/config/wireless"
#define WIFI_CMD_SSID             "uci get wireless.@wifi-iface[%d].ssid"
#define WIFI_CMD_KEY              "uci get wireless.@wifi-iface[%d].key"
#define KEY_MAX_LEN               6
#define BUFF_SIZE                 64

/* Possible commisioning authentication. */
enum commissioning_auth_t {
	COMMISSIONING_AUTH_NONE = 0x00,
	COMMISSIONING_AUTH_WIFI_CFG,
	COMMISSIONING_AUTH_MANUAL
};

/* Identificator of HCI device being used. */
static int dev_id = -1;

static volatile int signal_received;
static unsigned int scanning_window = DEFAULT_SCANNING_WINDOW;
static unsigned int scanning_interval = DEFAULT_SCANNING_INTERVAL;

/* Authentication parameters. */
static bool	     mgmt_initialized = false;
static struct mgmt   *mgmt;
static unsigned int  auth_type = COMMISSIONING_AUTH_NONE;
static char          auth_ssid_value[AUTH_SSID_MAX_LEN+1];
static unsigned int  auth_ssid_len;
static char          auth_key_value[AUTH_KEY_LEN+1];
static int	     auth_wifi_iface;

/* Help menu */
static void usage(void)
{
	printf("bluetooth_6lowpand ver 1.0.0\n");
	printf("Usage:\n"
		"\tbluetooth_6lowpand [options] <command> [command parameters]\n");
	printf("Options:\n"
		"\t--help\tDisplay help\n"
		"\t-i dev\tSet the HCI device. Default is hci0\n"
		"\t-t scanning interval\tSet the scanning interval. Default value is 10 seconds\n"
		"\t-w scanning window\tSet the scanning window. Default value is 5 seconds\n"
		"\t-W\tOnly scan the device in white list\n"
		"\t-a\tAuthentication of node.\tFormat SSID:KEY (e.g. OpenWRT:123456) else first WiFi configuration is used\n"
		"\t-n\tSet the WiFi instance. Default is 0\n"
		"\t-d\tDaemonize\n");
	printf("Commands:\n"
		"\taddwl\t[BDADDR]\tAdd device into white list\n"
		"\trmwl\t[BDADDR]\tRemove device into white list\n"
		"\tclearwl\t\t\tClear the content of white list\n"
		"\tlswl\t\t\tList the content of white list\n"
		"\tlscon\t\t\tList the 6lowpan connections\n");
}


/* Signal handler. */
static void sigint_handler(int sig)
{
	signal_received = sig;

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		mainloop_quit();
		break;
	}
}


/* Validate correctness of key, using in commissioning. */
static int validate_key(const char *key_value)
{
	int index;

	if (strlen(key_value) < AUTH_KEY_LEN)
		return -1;

	for (index = 0; index < AUTH_KEY_LEN; index++) {
		if (!isdigit(key_value[index]))
			return -1;
	}

	return 0;
}


/* Validate and store authentication parameters. */
static int validate_store_auth_params(const char *ssid_value, const char *key_value)
{
	int length;

	if (!ssid_value || !key_value) {
		perror("SSID and Key cannot be empty");
		return -1;
	}

	length = strlen(ssid_value);
	if (length > AUTH_SSID_MAX_LEN) {
		/* Use only AUTH_SSID_MAX_LEN, the most significant bytes. */
		length = AUTH_SSID_MAX_LEN;
	}

	/* Store SSID value. */
	memcpy(auth_ssid_value, ssid_value, length);
	auth_ssid_value[length] = 0;
	auth_ssid_len = length;

	/* Validate and store key. */
	if (validate_key(key_value) == -1) {
		perror("Key has to have 6 numeric character");
		return -1;
	}

	memcpy(auth_key_value, key_value, AUTH_KEY_LEN);
	auth_key_value[AUTH_KEY_LEN] = 0;

	return 0;
}


/* Read SSID and Key from command line parameters. */
static int read_manual_cfg(char *str)
{
	char *token = NULL;
	char ssid_value[BUFF_SIZE];
	char key_value[BUFF_SIZE];

	memset(ssid_value, 0, BUFF_SIZE);
	memset(key_value, 0, BUFF_SIZE);

	/* Get the SSID value */
	token = strtok(str, ":");
	if (token != NULL)
		memcpy(ssid_value, token, strlen(token));

	/* Get the KEY value */
	token = strtok(NULL, ":");
	if (token != NULL) {
		memcpy(key_value, token, AUTH_KEY_LEN);
	} else {
		perror("Key is required. Format SSID:PASSKEY e.g. SSID:123456");
		return -1;
	}

	/* Validate and store authentication parameters. */
	return validate_store_auth_params(ssid_value, key_value);
}


/* Read SSID and Key from WiFi configuration. */
static int read_wifi_cfg(void)
{
	FILE *fp;
	char path[64];
	char command[64];
	char ssid_value[BUFF_SIZE];
	char key_value[BUFF_SIZE];

	memset(ssid_value, 0, BUFF_SIZE);
	memset(key_value, 0, BUFF_SIZE);

	/* Read SSID value */
	snprintf(command, sizeof(command), WIFI_CMD_SSID, auth_wifi_iface);

	fp = popen(command, "r");
	if (fp == NULL) {
		perror("Failed to read SSID");
		return -1;
	}

	if (fgets(path, sizeof(path)-1, fp) != NULL) {
		memcpy(ssid_value, path, strlen(path)-1);
	} else {
		perror("Cannot found UCI SSID");
		return -1;
	}

	pclose(fp);

	/* Read Key value */
	snprintf(command, sizeof(command), WIFI_CMD_KEY, auth_wifi_iface);

	fp = popen(command, "r");
	if (fp == NULL) {
		perror("Failed to read KEY");
		return -1;
	}

	if (fgets(path, sizeof(path)-1, fp) != NULL) {
		memcpy(key_value, path, strlen(path)-1);
	} else {
		perror("Cannot found UCI KEY");
		return -1;
	}

	pclose(fp);

	/* Validate and store authentication parameters. */
	return validate_store_auth_params(ssid_value, key_value);
}


/* Connect the BLE 6lowpan device. */
static bool connect_device(char *addr, bool connect)
{
	int fd;
	bool ret = false;
	char command[64];

	fd = open(CONTROLLER_PATH, O_WRONLY);
	if (fd < 0) {
		perror("Can not open 6lowpan controller\n");
		return ret;
	}

	if (connect)
		snprintf(command, sizeof(command), "connect %s 1", addr);
	else
		snprintf(command, sizeof(command), "disconnect %s 1", addr);

	if (write(fd, command, sizeof(command)) > 0)
		ret = true;

	close(fd);

	return ret;
}


/*  Parse EIR events for IPSP device. */
static bool parse_ip_service(uint8_t *eir, size_t eir_len, char *buf, size_t buf_len)
{
	size_t offset = 0;
	bool ipsp_service = false;
	bool ssid_correct = false;
	uint8_t val[256];

	memset(val, 0, 256);

	while (offset < eir_len) {
		size_t field_len = eir[0];

		switch (eir[1]) {
		case EIR_UUID16_SOME:
		case EIR_UUID16_ALL:
			put_le16(IPSP_UUID, &val[0]);
			if (!memcmp(val, eir + 2, field_len/2))
				ipsp_service = true;
			break;
		case EIR_NAME_SHORT:
		case EIR_NAME_COMPLETE:
			if (field_len - 1 > buf_len)
				break;
			memcpy(buf, &eir[2], field_len - 1);
			break;
		case EIR_MANUF_SPECIFIC_DATA:
			put_le16(NORDIC_COMPANY_ID, &val[0]);
			if (auth_type == COMMISSIONING_AUTH_WIFI_CFG) {
				/* Reread configuration of WiFi. */
				if (read_wifi_cfg() == -1) {
					perror("Cannot read Wifi configuration.");
					return 0;
				}
			}

			memcpy(val + 2, auth_ssid_value, auth_ssid_len);
			if (!memcmp(val, eir + 2, auth_ssid_len + 2) &&
				auth_ssid_len == field_len - 3)
				ssid_correct = true;
			break;
		}

		offset += field_len + 1;
		eir += field_len + 1;
	}

	return ipsp_service && (ssid_correct || (auth_type == COMMISSIONING_AUTH_NONE));
}


/* Get the number of connected BLE devices */
static int current_conn_num(int dev_id)
{
	int sk, conn_num;
	struct hci_conn_list_req *cl;
	struct hci_conn_info *ci;

	sk = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	if (sk < 0) {
		perror("Can't allocate socket");
		return -1;
	}

	cl = malloc(MAX_BLE_CONN * sizeof(*ci) + sizeof(*cl));

	if (!cl) {
		perror("Can't allocate memory");
		return -1;
	}

	cl->dev_id = dev_id;
	cl->conn_num = MAX_BLE_CONN;

	if (ioctl(sk, HCIGETCONNLIST, (void *) cl)) {
		perror("Can't get connection list");
		return -1;
	}

	conn_num = cl->conn_num;
	free(cl);
	close(sk);

	return conn_num;
}


/* Check if whitelist contains target address. */
static bool check_whitelist(const char *target_addr)
{
	FILE *fp = NULL;
	char item[CONFIG_LINE_MAX];
	struct flock lock;

	while (access(CONFIG_SWP_PATH, F_OK) != -1) {
		/* Wait if swap file exists */
		sleep(1);
	}

	fp = fopen(CONFIG_PATH, "a+");
	if (!fp) {
		perror("Open config failed");
		return false;
	}

	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	lock.l_pid = getpid();

	if (((fcntl(fileno(fp), F_SETLK, &lock)) == -1) && ((errno == EAGAIN) || (errno == EACCES))) {
		printf("file is locked\n");
		fclose(fp);
		return false;
	}

	/* Check address in conf. */
	while ((fgets(item, sizeof(item), fp) != NULL)) {
		char *pch;

		pch = strchr(item, '"');
		if (pch) {
			if (!strncasecmp(pch+1, target_addr, 17)) {
				printf("%s is in white list\n", target_addr);
				fflush(fp);
				fsync(fileno(fp));
				fclose(fp);
				return true;
			}
		}
		memset(item, 0, sizeof(item));
	}

	printf("%s is not in white list\n", target_addr);
	fclose(fp);
	return false;
}


/* Scan the IPSP device */
static bool scan_ipsp_device(int dd, unsigned int timeout, char *dev_name, char *dev_addr, bool use_whitelist)
{
	/* device scan parameters */
	uint8_t own_type = LE_PUBLIC_ADDRESS;
	uint8_t scan_type = 0x01; /* Active scanning. */
	uint8_t filter_policy = 0x00;
	uint16_t interval = htobs(0x0010);
	uint16_t window = htobs(0x0004);
	uint8_t filter_dup = 0x01;

	unsigned char buf[HCI_MAX_EVENT_SIZE], *ptr;
	struct hci_filter nf, of;
	struct sigaction sa;
	socklen_t olen;
	struct pollfd pollfd;
	int poll_ret;
	bool scan_ret = false;
	int err;
	time_t start_time;
	time_t curr_time;
	double running_time;

	start_time = time(NULL);

	/* Scan BLE devices */
	err = hci_le_set_scan_parameters(dd, scan_type, interval, window, own_type, filter_policy, 10000);
	if (err < 0) {
		perror("Set scan parameters failed");
		return scan_ret;
	}

	err = hci_le_set_scan_enable(dd, 0x01, filter_dup, 10000);
	if (err < 0) {
		perror("Enable scan failed");
		return scan_ret;
	}

	DEBUG_PRINT("LE Scan ...\n");

	olen = sizeof(of);
	if (getsockopt(dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
		printf("Could not get socket options\n");
		goto done;
	}

	hci_filter_clear(&nf);
	hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
	hci_filter_set_event(EVT_LE_META_EVENT, &nf);

	if (setsockopt(dd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
		printf("Could not set socket options\n");
		goto done;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	while (timeout > 0) {
		evt_le_meta_event *meta;
		le_advertising_info *info;
		char addr[DEVICE_ADDR_LEN];
		char name[DEVICE_NAME_LEN];

		curr_time = time(NULL);
		running_time = difftime(curr_time, start_time);

		if (running_time > timeout)
			goto done;

		memset(name, 0, sizeof(name));
		memset(addr, 0, sizeof(addr));
		memset(buf, 0, sizeof(buf));

		pollfd.fd = dd;
		pollfd.events = POLLIN;

		poll_ret = poll(&pollfd, 1, (timeout - running_time)*1000);
		if (poll_ret < 0) {
			printf("poll hci dev error\n");
			goto done;
		} else if (poll_ret == 0) {
			/* poll timeout */
			goto done;
		} else {
			if (pollfd.revents & POLLIN) {
				while ((read(dd, buf, sizeof(buf))) < 0) {
					if (errno == EINTR && signal_received == SIGINT)
						goto done;

					if (errno == EAGAIN || errno == EINTR)
						continue;

					goto done;
				}
			}
		}

		ptr = buf + (1 + HCI_EVENT_HDR_SIZE);
		meta = (void *) ptr;

		if (meta->subevent != EVT_LE_ADVERTISING_REPORT)
			goto done;

		/* Ignoring multiple reports */
		info = (le_advertising_info *) (meta->data + 1);

		ba2str(&info->bdaddr, addr);
		if (parse_ip_service(info->data, info->length, name, sizeof(name) - 1)) {
			DEBUG_PRINT("Found IPSP supported device %s %s\n", name, addr);
			memcpy(dev_name, name, sizeof(name));
			memcpy(dev_addr, addr, sizeof(addr));
			if (use_whitelist && (check_whitelist(addr) == false)) {
				/* Nothing to do. Check whitelist for next entry. */
			} else {
				scan_ret = true;
				break;
			}
		} else {
			DEBUG_PRINT("IPSP not supported device %s %s\n", name, addr);
		}
	}

done:
	setsockopt(dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));
	err = hci_le_set_scan_enable(dd, 0x00, filter_dup, 10000);
	if (err < 0) {
		perror("Disable scan failed");
		scan_ret = false;
	}

	return scan_ret;
}


/* Management API passkey request. */
static void passkey_request_event(uint16_t index, uint16_t len,
				  const void *param, void *user_data)
{
	const struct mgmt_ev_pin_code_request *ev = param;
	static struct mgmt_cp_user_passkey_reply cp;
	char str[18];

	ba2str(&ev->addr.bdaddr, str);

	memset(&cp, 0, sizeof(cp));
	memcpy(&cp.addr, &ev->addr, sizeof(cp.addr));
	put_le32(atoi(auth_key_value), &cp.passkey);

	DEBUG_PRINT("Passkey request: %d\r\n", cp.passkey);

	/* Reply with correct passkey. */
	mgmt_reply(mgmt, MGMT_OP_USER_PASSKEY_REPLY, index, sizeof(cp), &cp,
		   NULL, NULL, NULL);
}


/* Management API configuration result. */
static void set_cfg_complete(uint8_t status, uint16_t len,
			     const void *param, void *user_data)
{
	const char *fn_name = user_data;

	if (status) {
		fprintf(stderr, "Configuration command %s failed - reason: %s\n",
				fn_name, mgmt_errstr(status));
		mainloop_quit();
		return;
	}
}


/* Management API pairing result. */
static void pair_device_complete(uint8_t status, uint16_t len,
				 const void *param, void *user_data)
{
	const struct mgmt_cp_pair_device *ev = param;

	char bastr[20];
	memset(bastr, 0, 20);

	if (status) {
#ifdef DEBUG_6LOWPAN
		fprintf(stderr, "Pair device from index %u failed: %s %d\n",
			PTR_TO_UINT(user_data), mgmt_errstr(status), status);
#endif
		mainloop_quit();
		return;
	}

	DEBUG_PRINT("Pair device complete!\r\n");

	/* Change BT-LE address to string object. */
	ba2str(&ev->addr.bdaddr, bastr);

	if (connect_device(bastr, true))
		printf("Device %s connect ok!\n", bastr);
	else
		printf("Device %s connect fail!\n", bastr);

	mainloop_quit();
}


/* Management API pair device. */
static void pair_device(uint16_t index, const bdaddr_t *bdaddr)
{
	struct mgmt_cp_pair_device cp;

#ifdef DEBUG_6LOWPAN
	char bastr[20];

	ba2str(bdaddr, bastr);
	DEBUG_PRINT("Starting pairing with node: %s\n", bastr);
#endif

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.addr.bdaddr, bdaddr);
	cp.addr.type = BDADDR_LE_PUBLIC;
	cp.io_cap = 0x02;

	mgmt_send(mgmt, MGMT_OP_PAIR_DEVICE, index, sizeof(cp), &cp,
		  pair_device_complete, UINT_TO_PTR(index), NULL);
}


/* Management API set powered complete event. */
static void set_powered_complete(uint8_t status, uint16_t len,
				 const void *param, void *user_data)
{
	uint16_t index = PTR_TO_UINT(user_data);
	uint32_t settings;

	if (status) {
		fprintf(stderr, "Powering on for index %u failed: %s\n",
						index, mgmt_errstr(status));
		mainloop_quit();
		return;
	}

	settings = get_le32(param);

	if (!(settings & MGMT_SETTING_POWERED)) {
		fprintf(stderr, "Controller is not powered\n");
		mainloop_quit();
		return;
	}

	mgmt_initialized = true;
	mainloop_quit();
}


/* Management API read/set hci interface capabilities event. */
static void read_info(uint8_t status, uint16_t len, const void *param,
		      void *user_data)
{
	const struct mgmt_rp_read_info *rp = param;
	uint16_t index = PTR_TO_UINT(user_data);
	uint32_t supported_settings;
	uint8_t val;

	if (status) {
		fprintf(stderr, "Reading info for index %u failed: %s\n",
				index, mgmt_errstr(status));
		mainloop_quit();
		return;
	}

	supported_settings = le32_to_cpu(rp->supported_settings);

	if (!(supported_settings & MGMT_SETTING_LE)) {
		fprintf(stderr, "Low Energy support missing\n");
		mainloop_quit();
		return;
	}

	val = 0x00;
	mgmt_send(mgmt, MGMT_OP_SET_POWERED, index, 1, &val,
		  set_cfg_complete, "MGMT_OP_SET_POWERED", NULL);

	val = 0x01;
	mgmt_send(mgmt, MGMT_OP_SET_LE, index, 1, &val,
		  set_cfg_complete, "MGMT_OP_SET_LE", NULL);

	val = 0x02;
	mgmt_send(mgmt, MGMT_OP_SET_IO_CAPABILITY, index, 1, &val,
		  set_cfg_complete, "MGMT_OP_SET_IO_CAPABILITY", NULL);

	val = 0x01;
	mgmt_send(mgmt, MGMT_OP_SET_POWERED, index, 1, &val,
		  set_powered_complete,	UINT_TO_PTR(index), NULL);
}


/* Initialize management API. */
static void comm_auth_init(void)
{
	mainloop_init();

	mgmt = mgmt_new_default();
	if (!mgmt) {
		fprintf(stderr, "Failed to open management socket\n");
		exit(0);
	}
}


/* Run the main management loop */
static void comm_auth_run(void)
{
	mainloop_run();
}


/* Configure management API to be able to pair devices. */
static void comm_auth_configure(void)
{
	if (!mgmt_send(mgmt, MGMT_OP_READ_INFO, dev_id, 0, NULL,
		       read_info, UINT_TO_PTR(dev_id), NULL)) {
		fprintf(stderr, "Failed to read index list\n");
		exit(0);
	}
}


/* Pair device using passkey authentication. */
static void comm_auth_pair(char *addr)
{
	bdaddr_t peeraddr;

	str2ba(addr, &peeraddr);

	/* Register user passkey request event. */
	mgmt_register(mgmt, MGMT_EV_USER_PASSKEY_REQUEST, dev_id,
		      passkey_request_event, UINT_TO_PTR(dev_id), NULL);

	pair_device(dev_id, &peeraddr);
}


/* main process to scan/connect all IPSP slaves */
static void process_6lowpan(char *hci_id, bool use_whitelist)
{
	int dd;

	dev_id = hci_devid(hci_id);
	if (dev_id < 0) {
		perror("Could not open device");
		exit(0);
	}

	DEBUG_PRINT("HCI Device ID = %d\r\n", dev_id);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		perror("Could not open device");
		exit(0);
	}

	if (auth_type != COMMISSIONING_AUTH_NONE) {
		comm_auth_init();
		comm_auth_configure();
		comm_auth_run();

		if (!mgmt_initialized) {
			perror("Could not initialize authentication");
			exit(0);
		}
	}

	while (1) {
		char addr[DEVICE_ADDR_LEN];
		char name[DEVICE_NAME_LEN];
		bool scan_ret;

		/* Scan the IPSP device */
		if (current_conn_num(dev_id) < MAX_BLE_CONN) {
			scan_ret = scan_ipsp_device(dd, scanning_window, name, addr, use_whitelist);
			if (scan_ret) {
				if (auth_type != COMMISSIONING_AUTH_NONE) {
					DEBUG_PRINT("Pairing with device %s\r\n", addr);

					comm_auth_init();
					comm_auth_pair(addr);
					comm_auth_run();
				} else {
					if (connect_device(addr, true))
						printf("Device %s connect ok!\n", addr);
					else
						printf("Device %s connect fail!\n", addr);
				}
			}
		}

		/* Stop running if the daemon receives interrupt signal */
		if (signal_received == SIGINT) {
			break;
		}

		/* Wait for scanning_interval second till next scanning */
		sleep(scanning_interval);
	}

	if (dd >= 0)
		hci_close_dev(dd);

	if (auth_type != COMMISSIONING_AUTH_NONE)
		mgmt_unref(mgmt);

	return;
}


/* Add device into white list */
static void cmd_addwl(char *argv)
{
	FILE *fp = NULL;
	char item[CONFIG_LINE_MAX];
	struct flock lock;

	DEBUG_PRINT("Add %s to white list\n", argv);

	if (strlen(argv) != 17) {
		perror("input address not correct");
		return;
	}

	while (access(CONFIG_SWP_PATH, F_OK) != -1) {
		/* Wait if swap file exists */
		sleep(1);
	}

	do {
		if (fp != NULL)
			fclose(fp);

		fp = fopen(CONFIG_PATH, "a+");
		if (!fp) {
			perror("Open config failed");
			return;
		}

		lock.l_type = F_WRLCK;
		lock.l_start = 0;
		lock.l_whence = SEEK_SET;
		lock.l_len = 0;
		lock.l_pid = getpid();
	} while ((fcntl(fileno(fp), F_SETLK, &lock) == -1) && ((errno == EAGAIN) || (errno == EACCES)));

	/* Find address in 6lowpan.conf */
	while (fgets(item, sizeof(item), fp)) {
		char *pch;

		/* Stop adding address if the address already exist */
		pch = strstr(item, "address");
		if (pch) {
			if (!strncasecmp(pch+9, argv, 17)) {
				DEBUG_PRINT("address is already in white list\n");
				fclose(fp);
				return;
			}
		}
		memset(item, 0, sizeof(item));
	}

	fprintf(fp, "{\n\taddress=\"%s\"\n}\n", argv);
	fflush(fp);
	fsync(fileno(fp));
	fclose(fp);
}


/* Remove device into white list */
static void cmd_rmwl(char *argv)
{
	FILE *fp_cur = NULL, *fp_swp = NULL;
	char item[CONFIG_LINE_MAX];
	struct flock lock_cur, lock_swp;

	DEBUG_PRINT("Remove %s from white list\n", argv);

	if (strlen(argv) != 17) {
		perror("input address not correct");
		return;
	}

	while (access(CONFIG_SWP_PATH, F_OK) != -1) {
		/* Wait if swap file exists */
		sleep(1);
	}

	do {
		if (fp_cur != NULL) {
			fclose(fp_cur);
		}

		fp_cur = fopen(CONFIG_PATH, "a+");
		if (!fp_cur) {
			perror("Open config failed");
			goto done;
		}

		lock_cur.l_type = F_WRLCK;
		lock_cur.l_start = 0;
		lock_cur.l_whence = SEEK_SET;
		lock_cur.l_len = 0;
		lock_cur.l_pid = getpid();
	} while ((fcntl(fileno(fp_cur), F_SETLK, &lock_cur) == -1) && ((errno == EAGAIN) || (errno == EACCES)));

	do {
		if (fp_swp != NULL)
			fclose(fp_swp);

		fp_swp = fopen(CONFIG_SWP_PATH, "w");
		if (!fp_swp) {
			perror("Open swap config failed");
			goto done;
		}

		lock_swp.l_type = F_WRLCK;
		lock_swp.l_start = 0;
		lock_swp.l_whence = SEEK_SET;
		lock_swp.l_len = 0;
		lock_swp.l_pid = getpid();
	} while ((fcntl(fileno(fp_swp), F_SETLK, &lock_swp) == -1) && ((errno == EAGAIN) || (errno == EACCES)));

	/* check address in conf */
	while (fgets(item, sizeof(item), fp_cur)) {
		if (item[0] == '{') {
			char *pch;
			memset(item, 0, sizeof(item));
			while (fgets(item, sizeof(item), fp_cur)) {
				if (item[0] == '}')
					break;

				pch = strstr(item, "address");
				if (pch) {
					if (!strncasecmp(pch+9, argv, 17))
						continue;
					else
						fprintf (fp_swp, "{\n%s}\n", item);
				}
			}
		}
		memset(item, 0, sizeof(item));
	}

done:
	if (!fp_cur) {
		fflush(fp_cur);
		fsync(fileno(fp_cur));
		fclose(fp_cur);
	}
	if (!fp_swp) {
		fflush(fp_swp);
		fsync(fileno(fp_swp));
		fclose(fp_swp);
	}

	if (rename(CONFIG_SWP_PATH, CONFIG_PATH) == -1)
		perror("Rename Fail");

	if (connect_device(argv, false))
		printf("Device %s disconnect ok!\n", argv);
	else
		printf("Device %s disconnect fail!\n", argv);
}


/* Clear the content of white list */
static void cmd_clearwl(char *argv)
{
	FILE *fp = NULL;

	DEBUG_PRINT("Clear white list\n");

	while (access(CONFIG_SWP_PATH, F_OK) != -1) {
		/* Wait if swap file exists */
		sleep(1);
	}

	fp = fopen(CONFIG_PATH, "w");
	if (!fp) {
		perror("Open config failed");
		return;
	}

	fclose(fp);
}


/* List the content of white list */
static void cmd_lswl(char *argv)
{
	FILE *fp = NULL;
	char item[CONFIG_LINE_MAX];
	struct flock lock;

	DEBUG_PRINT("List the white list\n");

	while (access(CONFIG_SWP_PATH, F_OK) != -1) {
		/* Wait if swap file exists */
		sleep(1);
	}

	do {
		if (fp != NULL)
			fclose(fp);

		fp = fopen(CONFIG_PATH, "a+");
		if (!fp) {
			perror("Open config failed");
			return;
		}

		lock.l_type = F_WRLCK;
		lock.l_start = 0;
		lock.l_whence = SEEK_SET;
		lock.l_len = 0;
		lock.l_pid = getpid();
	} while ((fcntl(fileno(fp), F_SETLK, &lock) == -1) && ((errno == EAGAIN) || (errno == EACCES)));

	/* Find address in 6lowpan.conf */
	while (fgets(item, sizeof(item), fp)) {
		char *pch;
		char bd_addr[18];

		pch = strstr(item, "address");
		if (pch) {
			if (strncpy(bd_addr, pch+9, sizeof(bd_addr)) != NULL) {
				bd_addr[17] = '\0';
				printf("%s\n", bd_addr);
			}
		}
		memset(item, 0, sizeof(item));
	}

	fclose(fp);
}


/* List the 6lowpan connections */
static void cmd_lscon(char *argv)
{
	/* Read current connections from controller */
	/* Format "00:11:22:33:44:55 (type 1) */
	/* Allocate 27 byte for each connection */
	char buffer[27*MAX_BLE_CONN];
	char *pch;
	int fd;

	memset(buffer, 0, sizeof(buffer));
	fd = open(CONTROLLER_PATH, O_RDONLY);
	if (fd < 0) {
		perror("Can not open 6lowpan controller");
		return;
	}

	if (read(fd, buffer, sizeof(buffer)) > 0) {
		pch = strtok(buffer, " \n");
		while (pch != NULL) {
			if (strlen(pch) == 17)
				printf ("%s\n", pch);
			pch = strtok (NULL, " \n");
		}
	}

	close(fd);
	return;
}


/* Commands */
static struct {
	char *cmd;
	void (*func)(char *argv);
	char *doc;
} command[] = {
	{ "addwl",	cmd_addwl,		"Add device into white list"	},
	{ "rmwl",	cmd_rmwl,		"Remove device from white list"	},
	{ "clearwl",	cmd_clearwl,		"Clear the white list"		},
	{ "lswl",	cmd_lswl,		"List the white list"		},
	{ "lscon",	cmd_lscon,		"List the 6lowpan connections"	},
	{0}
};

/* Options */
static struct option main_options[] = {
	{ "device",		 1, 0, 'i'},
	{ "use-whitelist",	 0, 0, 'W'},
	{ "scanning window",	 1, 0, 'w'},
	{ "scanning interval",	 1, 0, 't'},
	{ "wifi",		 1, 0, 'n'},
	{ "authentication",      2, 0, 'a'},
	{ "daemonize",		 0, 0, 'd'},
	{ "help",		 0, 0, 'h'},
	{0}
};


int main(int argc, char *argv[])
{
	int opt, i, j, optindex;
	char *hci_id = NULL;
	bool use_whitelist = false, daemonize = false;

	while ((opt = getopt_long(argc, argv, "i:Ww:t:dhn:a::", main_options, &optindex)) != -1) {
		switch (opt) {
		case 'i':
			printf("Use hci interface: %s\n", optarg);
			hci_id = optarg;
			break;
		case 'W':
			printf("use white list\n");
			use_whitelist = true;
			break;
		case 'w':
			{
				unsigned int input_window;
				input_window = atoi(optarg);
				printf("Set scanning window to %d\n", input_window);
				if (input_window > 0 && input_window <= MAX_SCANNING_WINDOW) {
					scanning_window = input_window;
				} else {
					perror("Interval should be between 0 ~ 30 seconds");
					exit(-1);
				}
			}
			break;
		case 't':
			{
				unsigned int input_interval;
				input_interval = atoi(optarg);
				printf("Set scanning interval to %d\n", input_interval);
				if (input_interval > 0 && input_interval <= MAX_SCANNING_INTERVAL) {
					scanning_interval = input_interval;
				} else {
					perror("Interval should be between 0 ~ 300 seconds");
					exit(-1);
				}
			}
			break;
		case 'a':
			{
				char *auth_optarg = optarg;

				if (!optarg && NULL != argv[optind] && '-' != argv[optind][0])
					auth_optarg = argv[optind++];

				if (auth_optarg) {
					if (read_manual_cfg(auth_optarg) == -1)	{
						perror("Cannot read authentication configuration. Use SSID:PASSKEY syntax.");
						exit(-1);
					}

					auth_type = COMMISSIONING_AUTH_MANUAL;

				} else {
					auth_type = COMMISSIONING_AUTH_WIFI_CFG;
				}

				DEBUG_PRINT("Authentication parameteres:\r\nSSID:\t%s\r\nKEY:\t%s\r\nTYPE:\t%d\r\n", auth_ssid_value, auth_key_value, auth_type);
			}
			break;
		case 'd':
			printf("Daemonize\n");
			daemonize = true;
			break;
		case 'n':
			auth_wifi_iface = atoi(optarg);
			printf("Use WiFi interface: %d\n", auth_wifi_iface);
			break;
		case 'h':
		default:
			usage();
			exit(0);
		}
	}

	if(auth_type == COMMISSIONING_AUTH_WIFI_CFG)
	{
		if (read_wifi_cfg() == -1) {
			perror("Cannot read Wifi configuration.");
			exit(-1);
		}
	}

	for (i = 0; i < argc ; i++) {
		for (j = 0; command[j].cmd; j++) {
			if (strncmp(command[j].cmd, argv[i], strlen(command[j].cmd)))
				continue;
			command[j].func(argv[i+1]);
			exit(0);
		}
	}

	if (daemonize) {
		if (daemon(0, 0)) {
			printf("Failed to daemonize: %s",
				strerror(errno));
			return 0;
		}
	}

	if (hci_id != NULL) {
		printf("Run 6lowpan on interface %s\n", hci_id);
		process_6lowpan(hci_id, use_whitelist);
	} else {
		printf("Run 6lowpan on default interface hci0\n");
		process_6lowpan("hci0", use_whitelist);
	}


	return 0;
}

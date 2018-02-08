/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2018, CESAR. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <glib.h>
#include <ell/ell.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "hal/nrf24.h"
#include "hal/comm.h"
#include "hal/time.h"

#include "hal/linux_log.h"
#include "manager.h"

#define KNOTD_UNIX_ADDRESS		"knot"
#define MAC_ADDRESS_SIZE		24
#define BCAST_TIMEOUT			10000

#ifndef MIN
#define MIN(a,b)			(((a) < (b)) ? (a) : (b))
#endif

static int mgmtfd;
static guint mgmtwatch;
static struct in_addr inet_address;
static int tcp_port;

struct l_dbus *g_dbus = NULL;

static struct adapter {
	struct nrf24_mac mac;

	/* File with struct keys */
	gchar *keys_pathname;
	gboolean powered;

	struct l_queue *peer_offline_list; /* Disconnected */
	struct l_queue *peer_online_list; /* Connected */
	struct l_queue *beacon_list;
} adapter;

struct peer {
	struct nrf24_mac addr;
	char *alias;
	int8_t socket_fd; /* HAL comm socket */
	int8_t ksock; /* KNoT raw socket: Unix socket or TCP */
	guint kwatch; /* KNoT raw socket watch */
};

struct beacon {
	struct nrf24_mac addr;
	char *name;
	unsigned long last_beacon;
};

static void beacon_free(void *user_data)
{
	struct beacon *peer = user_data;

	g_free(peer->name);
	g_free(peer);
}

static void peer_free(void *data)
{
	struct peer *peer = data;

	l_free(peer->alias);

	l_free(peer);
}

static bool beacon_match(const void *a, const void *b)
{
	const struct beacon *beacon = a;
	const struct nrf24_mac *addr = b;
	int ret = memcmp(&beacon->addr, addr, sizeof(beacon->addr));

	return (ret ? false : true);
}

static bool beacon_if_expired(const void *data, const void *user_data)
{
	const struct beacon *peer = data;
	const int *ms = user_data;

	/* If it returns true the key/value is removed */
	if (hal_timeout(*ms, peer->last_beacon, BCAST_TIMEOUT) > 0)
		return true;

	return false;
}

static bool peer_match(const void *a, const void *b)
{
	const struct peer *peer = a;
	const struct nrf24_mac *addr = b;
	int ret = memcmp(&peer->addr, addr, sizeof(peer->addr));

	return (ret ? false : true);
}

static int write_file(const gchar *addr, const gchar *key, const gchar *name)
{
	int array_len;
	int i;
	int err = -EINVAL;
	json_object *jobj, *jobj2;
	json_object *obj_keys, *obj_array, *obj_tmp, *obj_mac;

	/* Load nodes' info from json file */
	jobj = json_object_from_file(adapter.keys_pathname);
	if (!jobj)
		return -EINVAL;

	if (!json_object_object_get_ex(jobj, "keys", &obj_keys))
		goto failure;

	array_len = json_object_array_length(obj_keys);
	/*
	 * If name and key are NULL it means to remove element
	 * If only name is NULL, update some element
	 * Otherwise add some element to file
	 */
	if (name == NULL && key == NULL) {
		jobj2 = json_object_new_object();
		obj_array = json_object_new_array();
		for (i = 0; i < array_len; i++) {
			obj_tmp = json_object_array_get_idx(obj_keys, i);
			if (!json_object_object_get_ex(obj_tmp, "mac",
								&obj_mac))
				goto failure;

		/* Parse mac address string into struct nrf24_mac known_peers */
			if (g_strcmp0(json_object_get_string(obj_mac), addr)
									!= 0)
				json_object_array_add(obj_array,
						json_object_get(obj_tmp));
		}
		json_object_object_add(jobj2, "keys", obj_array);
		json_object_to_file(adapter.keys_pathname, jobj2);
		json_object_put(jobj2);
	} else if (name == NULL) {
	/* TODO update key of some mac (depends on adding keys to file) */
	} else {
		obj_tmp = json_object_new_object();
		json_object_object_add(obj_tmp, "name",
						json_object_new_string(name));
		json_object_object_add(obj_tmp, "mac",
						json_object_new_string(addr));
		json_object_array_add(obj_keys, obj_tmp);
		json_object_to_file(adapter.keys_pathname, jobj);
	}

	err = 0;
failure:
	json_object_put(jobj);
	return err;
}

static void dbus_disconnect_callback(void *user_data)
{
	hal_log_info("D-Bus disconnected");
}

static void dbus_request_name_callback(struct l_dbus *dbus, bool success,
					bool queued, void *user_data)
{
	if (!success)
		hal_log_error("Name request failed");
}

static void dbus_ready_callback(void *user_data)
{
	l_dbus_name_acquire(g_dbus, "org.cesar.knot.nrf", false, false, true,
			    dbus_request_name_callback, NULL);

	if (!l_dbus_object_manager_enable(g_dbus))
		hal_log_error("Unable to register the ObjectManager");
}

static void dbus_start(void)
{
	g_dbus = l_dbus_new_default(L_DBUS_SYSTEM_BUS);

	l_dbus_set_ready_handler(g_dbus, dbus_ready_callback,
				 g_dbus, NULL);
	l_dbus_set_disconnect_handler(g_dbus, dbus_disconnect_callback,
				      NULL, NULL);
}

static int unix_connect(void)
{
	struct sockaddr_un addr;
	int sock;

	sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (sock < 0)
		return -errno;

	/* Represents unix socket from nrfd to knotd */
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path + 1, KNOTD_UNIX_ADDRESS,
					strlen(KNOTD_UNIX_ADDRESS));

	if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1)
		return -errno;

	return sock;
}

static int tcp_init(const char *host)
{
	struct hostent *hostent;		/* Host information */
	int err;

	hostent = gethostbyname(host);
	if (hostent == NULL) {
		err = errno;
		hal_log_error("gethostbyname(): %s(%d)", strerror(err), err);
		return -err;
	}

	inet_address.s_addr = *((unsigned long *) hostent-> h_addr_list[0]);

	return 0;
}

static int tcp_connect(void)
{
	struct sockaddr_in server;
	int err, sock, enable = 1;

	sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		err = errno;
		hal_log_error("socket(): %s(%d)", strerror(err), err);
		return -err;
	}

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_address.s_addr;
	server.sin_port = htons(tcp_port);

	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &enable,
						sizeof(enable)) == -1) {
		err = errno;
		hal_log_error("tcp setsockopt(iTCP_NODELAY): %s(%d)",
							strerror(err), err);
		close(sock);
		return -err;
	}

	err = connect(sock, (struct sockaddr *) &server, sizeof(server));
	if (err < 0)
		return -errno;

	return sock;
}

static void kwatch_io_destroy(gpointer user_data)
{
	struct peer *p = (struct peer *) user_data;

	hal_comm_close(p->socket_fd);
	close(p->ksock);
	p->socket_fd = -1;
	p->kwatch = 0;

	if (!l_queue_remove(adapter.peer_online_list, p))
		return;


	l_queue_push_head(adapter.peer_offline_list, p);
}

static gboolean kwatch_io_read(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct peer *p = (struct peer *) user_data;
	GError *gerr = NULL;
	GIOStatus status;
	char buffer[128];
	size_t rx;
	ssize_t tx;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		return FALSE;

	/* Reading data from knotd */
	status = g_io_channel_read_chars(io, buffer, sizeof(buffer),
								&rx, &gerr);
	if (status != G_IO_STATUS_NORMAL) {
		hal_log_error("glib read(): %s", gerr->message);
		g_error_free(gerr);
		return FALSE;
	}

	/* Send data to thing */
	/* TODO: put data in list for transmission */

	tx = hal_comm_write(p->socket_fd, buffer, rx);
	if (tx < 0)
		hal_log_error("hal_comm_write(): %zd", tx);

	return TRUE;
}

static int8_t evt_presence(struct mgmt_nrf24_header *mhdr, ssize_t rbytes)
{
	GIOCondition cond = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	GIOChannel *io;
	int8_t position;
	uint8_t i;
	int sock, nsk;
	char mac_str[MAC_ADDRESS_SIZE];
	struct beacon *beacon;
	struct peer *peer;
	struct mgmt_evt_nrf24_bcast_presence *evt_pre =
			(struct mgmt_evt_nrf24_bcast_presence *) mhdr->payload;
	ssize_t name_len;

	nrf24_mac2str(&evt_pre->mac, mac_str);
	beacon = l_queue_find(adapter.beacon_list, beacon_match, &evt_pre->mac);
	if (beacon != NULL) {
		beacon->last_beacon = hal_time_ms();
		goto done;
	}
	beacon = g_try_new0(struct beacon, 1);
	if (beacon == NULL)
		return -ENOMEM;
	/*
	 * Print every MAC sending presence in order to ease the discover of
	 * things trying to connect to the gw.
	 */
	beacon->addr = evt_pre->mac;
	beacon->last_beacon = hal_time_ms();
	/*
	 * Calculating the size of the name correctly: rbytes contains the
	 * amount of data received and this contains two structures:
	 * mgmt_nrf24_header & mgmt_evt_nrf24_bcast_presence.
	 */
	name_len = rbytes - sizeof(*mhdr) - sizeof(*evt_pre);

	/* Creating a UTF-8 copy of the name */
	beacon->name = g_utf8_make_valid((const char *) evt_pre->name, name_len);
	if (!beacon->name)
		beacon->name = l_strdup("unknown");

	hal_log_info("Thing sending presence. MAC = %s Name = %s",
						mac_str, beacon->name);
	/*
	 * MAC and device name will be printed only once, but the last presence
	 * time is updated. Every time a user refresh the list in the webui
	 * we will discard devices that broadcasted
	 */
	l_queue_push_head(adapter.beacon_list, beacon);
done:
	/* Check if peer belongs to this gateway */
	peer = l_queue_find(adapter.peer_offline_list,
			    peer_match, &evt_pre->mac);
	if (!peer)
		return -EPERM;

	if (l_queue_length(adapter.peer_online_list) == MAX_PEERS)
		return -EUSERS; /* MAX PEERS: No room for more connection */

	/* Check if this peer is already allocated */
	peer = l_queue_find(adapter.peer_online_list, peer_match, &evt_pre->mac);
	if (peer) {
		hal_log_info("Attack: MAC clonning");
		return -EPERM;
	}

	/* Radio socket: nRF24 */
	nsk = hal_comm_socket(HAL_COMM_PF_NRF24, HAL_COMM_PROTO_RAW);
	if (nsk < 0) {
		hal_log_error("hal_comm_socket(nRF24): %s(%d)",
			      strerror(nsk), nsk);
		return nsk;
	}

	/* Upper layer socket: knotd */
	if (inet_address.s_addr)
		sock = tcp_connect();
	else
		sock = unix_connect();

	if (sock < 0) {
		hal_log_error("connect(): %s(%d)", strerror(sock), sock);
		hal_comm_close(nsk);
		return sock;
	}

	peer = l_new(struct peer, 1);
	peer->alias = l_strdup(beacon->name);
	peer->addr = evt_pre->mac;
	peer->ksock = sock;
	peer->socket_fd = nsk;

	/* FIXME: Watch knotd socket */
	io = g_io_channel_unix_new(peer->ksock);
	g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK, NULL);
	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_set_encoding(io, NULL, NULL);
	g_io_channel_set_buffered(io, FALSE);

	peer->kwatch = g_io_add_watch_full(io,
					  G_PRIORITY_DEFAULT,
					  cond,
					  kwatch_io_read,
					  peer,
					  kwatch_io_destroy);
	g_io_channel_unref(io);

	/* Remove device when the connection is established */
	l_queue_remove_if(adapter.beacon_list, beacon_match, &evt_pre->mac);

	/* Send Connect */
	return hal_comm_connect(peer->socket_fd,
			&evt_pre->mac.address.uint64);
}

static void evt_disconnected(struct mgmt_nrf24_header *mhdr)
{
	char mac_str[MAC_ADDRESS_SIZE];
	int8_t position;

	struct mgmt_evt_nrf24_disconnected *evt_disc =
			(struct mgmt_evt_nrf24_disconnected *) mhdr->payload;

	nrf24_mac2str(&evt_disc->mac, mac_str);
	hal_log_info("Peer disconnected(%s)", mac_str);

	l_queue_remove_if(adapter.peer_online_list, peer_match, &evt_disc->mac);
}

static void peer_read(void *data, void *user_data)
{
	uint8_t buffer[256];
	int rx, err, i;
	struct peer *p = data;

	if (p->socket_fd == -1)
		return;

	rx = hal_comm_read(p->socket_fd, &buffer, sizeof(buffer));
	if (rx > 0) {
		if (write(p->ksock, buffer, rx) < 0) {
			err = errno;
			hal_log_error("write to knotd: %s(%d)",
				      strerror(err), err);
		}
	}
}

static int8_t mgmt_read(void)
{
	uint8_t buffer[256];
	struct mgmt_nrf24_header *mhdr = (struct mgmt_nrf24_header *) buffer;
	ssize_t rbytes;

	memset(buffer, 0x00, sizeof(buffer));
	rbytes = hal_comm_read(mgmtfd, buffer, sizeof(buffer));

	/* mgmt on bad state? */
	if (rbytes < 0 && rbytes != -EAGAIN)
		return rbytes;

	/* Nothing to read? */
	if (rbytes == -EAGAIN)
		return rbytes;

	/* Return/ignore if it is not an event? */
	if (!(mhdr->opcode & 0x0200))
		return -EPROTO;

	switch (mhdr->opcode) {

	case MGMT_EVT_NRF24_BCAST_PRESENCE:
		evt_presence(mhdr, rbytes);
		break;

	case MGMT_EVT_NRF24_BCAST_SETUP:
		break;

	case MGMT_EVT_NRF24_BCAST_BEACON:
		break;

	case MGMT_EVT_NRF24_DISCONNECTED:
		evt_disconnected(mhdr);
		break;
	}

	return 0;
}

static gboolean read_idle(gpointer user_data)
{
	mgmt_read();
	l_queue_foreach(adapter.peer_online_list, peer_read, NULL);

	return TRUE;
}

static int radio_init(const char *spi, uint8_t channel, uint8_t rfpwr,
						const struct nrf24_mac *mac)
{
	const struct nrf24_config config = {
			.mac = *mac,
			.channel = channel,
			.name = "nrf0" };
	int err;

	err = hal_comm_init("NRF0", &config);
	if (err < 0) {
		hal_log_error("Cannot init NRF0 radio. (%d)", err);
		return err;
	}

	mgmtfd = hal_comm_socket(HAL_COMM_PF_NRF24, HAL_COMM_PROTO_MGMT);
	if (mgmtfd < 0) {
		hal_log_error("Cannot create socket for radio (%d)", mgmtfd);
		goto done;
	}

	mgmtwatch = g_idle_add(read_idle, NULL);
	hal_log_info("Radio initialized");

	return 0;
done:
	hal_comm_deinit();

	return mgmtfd;
}

static void radio_stop(void)
{
	/* TODO: disconnect clients */
	hal_comm_close(mgmtfd);
	if (mgmtwatch)
		g_source_remove(mgmtwatch);
	hal_comm_deinit();
}

static char *load_config(const char *file)
{
	char *buffer;
	int length;
	FILE *fl = fopen(file, "r");

	if (fl == NULL) {
		hal_log_error("No such file available: %s", file);
		return NULL;
	}

	fseek(fl, 0, SEEK_END);
	length = ftell(fl);
	fseek(fl, 0, SEEK_SET);

	buffer = (char *) malloc((length+1)*sizeof(char));
	if (buffer) {
		fread(buffer, length, 1, fl);
		buffer[length] = '\0';
	}
	fclose(fl);

	return buffer;
}

/* Set TX Power from dBm to values defined at nRF24 datasheet */
static uint8_t dbm_int2rfpwr(int dbm)
{
	switch (dbm) {

	case 0:
		return NRF24_PWR_0DBM;

	case -6:
		return NRF24_PWR_6DBM;

	case -12:
		return NRF24_PWR_12DBM;

	case -18:
		return NRF24_PWR_18DBM;
	}

	/* Return default value when dBm value is invalid */
	return NRF24_PWR_0DBM;
}

static int gen_save_mac(const char *config, const char *file,
							struct nrf24_mac *mac)
{
	json_object *jobj, *obj_radio, *obj_tmp;

	int err = -EINVAL;

	jobj = json_tokener_parse(config);
	if (jobj == NULL)
		return -EINVAL;

	if (!json_object_object_get_ex(jobj, "radio", &obj_radio))
		goto done;

	if (json_object_object_get_ex(obj_radio,  "mac", &obj_tmp)) {

			char mac_string[MAC_ADDRESS_SIZE];
			mac->address.uint64 = 0;

			hal_getrandom(mac->address.b, sizeof(*mac));

			err = nrf24_mac2str(mac, mac_string);
			if (err == -1)
				goto done;

			json_object_object_add(obj_radio, "mac",
					json_object_new_string(mac_string));

			json_object_to_file((char *) file, jobj);
	}

	/* Success */
	err = 0;

done:
	/* Free mem used in json parse: */
	json_object_put(jobj);
	return err;
}

/*
 * TODO: Get "host", "spi" and "port"
 * parameters when/if implemented
 * in the json configuration file
 */
static int parse_config(const char *config, int *channel, int *dbm,
							struct nrf24_mac *mac)
{
	json_object *jobj, *obj_radio, *obj_tmp;

	int err = -EINVAL;

	jobj = json_tokener_parse(config);
	if (jobj == NULL)
		return -EINVAL;

	if (!json_object_object_get_ex(jobj, "radio", &obj_radio))
		goto done;

	if (json_object_object_get_ex(obj_radio, "channel", &obj_tmp))
		*channel = json_object_get_int(obj_tmp);

	if (json_object_object_get_ex(obj_radio,  "TxPower", &obj_tmp))
		*dbm = json_object_get_int(obj_tmp);

	if (json_object_object_get_ex(obj_radio,  "mac", &obj_tmp)) {
		if (json_object_get_string(obj_tmp) != NULL) {
			err =
			nrf24_str2mac(json_object_get_string(obj_tmp), mac);
			if (err == -1)
				goto done;
		}
	}

	/* Success */
	err = 0;

done:
	/* Free mem used in json parse: */
	json_object_put(jobj);
	return err;
}

/*
 * Reads the keys.json file to create the list of allowed peers.
 * If the file does not exist or is in the wrong format, a new one (empty)
 * is created.
 */
static int parse_nodes(const char *nodes_file)
{
	int array_len;
	int i;
	int err = -EINVAL;
	json_object *jobj;
	json_object *obj_keys, *obj_nodes, *obj_tmp;
	struct peer *peer;
	struct nrf24_mac addr;
	FILE *fp;

	/* Load nodes' info from json file */
	jobj = json_object_from_file(nodes_file);
	if (!jobj) {
		fp = fopen(nodes_file, "w");
		if (!fp) {
			hal_log_error("Could not create file %s", nodes_file);
			goto done;
		}
		fprintf(fp, "{\"keys\":[]}");
		fclose(fp);
		err = 0;
		goto done;
	}

	if (!json_object_object_get_ex(jobj, "keys", &obj_keys)){
		fp = fopen(nodes_file, "w");
		if (!fp){
			hal_log_error("Could not write file %s", nodes_file);
			goto done;
		}
		fprintf(fp, "{\"keys\":[]}");
		fclose(fp);
		err = 0;
		goto done;
	}

	array_len = json_object_array_length(obj_keys);
	if (array_len > MAX_PEERS) {
		hal_log_error("Too many nodes at %s. Loading %d of %d",
			      nodes_file, array_len, MAX_PEERS);
		array_len = MAX_PEERS;
	}

	for (i = 0; i < array_len; i++) {
		obj_nodes = json_object_array_get_idx(obj_keys, i);
		if (!json_object_object_get_ex(obj_nodes, "mac", &obj_tmp))
			goto done;

		/* Parse mac address string into struct nrf24_mac known_peers */
		if (nrf24_str2mac(json_object_get_string(obj_tmp), &addr) < 0)
			goto done;

		if (!json_object_object_get_ex(obj_nodes, "name", &obj_tmp))
			goto done;

		/* Set the name of the peer registered */
		peer = l_new(struct peer, 1);
		peer->alias = l_strdup(json_object_get_string(obj_tmp));
		peer->addr = addr;

		l_queue_push_head(adapter.peer_offline_list, peer);
	}

	err = 0;
done:
	/* Free mem used to parse json */
	json_object_put(jobj);
	return err;
}

static void beacon_timeout_cb(struct l_timeout *timeout, void *user_data)
{
	int ms = hal_time_ms();

	l_queue_remove_if(adapter.beacon_list, beacon_if_expired, &ms);
}

int manager_start(const char *file, const char *host, int port,
					const char *spi, int channel, int dbm,
					const char *nodes_file)
{
	int cfg_channel = 76, cfg_dbm = 0;
	char *json_str;
	struct nrf24_mac mac = {.address.uint64 = 0};
	int err = -1;

	/* Command line arguments have higher priority */
	json_str = load_config(file);
	if (json_str == NULL) {
		hal_log_error("load_config()");
		return err;
	}

	/* TODO: Add name to config file */
	err = parse_config(json_str, &cfg_channel, &cfg_dbm, &mac);
	if (err < 0) {
		hal_log_error("parse_config(): %d", err);
		free(json_str);
		return err;
	}

	memset(&adapter, 0, sizeof(struct adapter));
	adapter.peer_offline_list = l_queue_new();
	adapter.peer_online_list = l_queue_new();
	adapter.beacon_list = l_queue_new();

	/* Parse nodes info from nodes_file and writes it to known_peers */
	err = parse_nodes(nodes_file);
	if (err < 0) {
		hal_log_error("parse_nodes(): %d", err);
		free(json_str);
		return err;
	}

	if (mac.address.uint64 == 0)
		err = gen_save_mac(json_str, file, &mac);

	free(json_str);
	adapter.keys_pathname = g_strdup(nodes_file);
	adapter.mac = mac;
	adapter.powered = TRUE;

	if (err < 0) {
		hal_log_error("Invalid configuration file(%d): %s", err, file);
		return err;
	}

	/*
	 * Priority order: 1) command line 2) config file.
	 * If the user does not provide channel at command line (or channel is
	 * invalid), switch to channel informed at config file. 76 is the
	 * default vale if channel in not informed in the config file.
	 */
	if (channel < 0 || channel > 125)
		channel = cfg_channel;

	/*
	 * Use TX Power from configuration file if it has not been passed
	 * through cmd line. -255 means invalid: not informed by user.
	 */
	if (dbm == -255)
		dbm = cfg_dbm;

	/* Start server dbus */
	dbus_start();

	/* TCP development mode: RPi(nrfd) connected to Linux(knotd) */
	if (host) {
		memset(&inet_address, 0, sizeof(inet_address));
		err = tcp_init(host);
		if (err < 0)
			return err;

		tcp_port = port;
	}

	err = radio_init(spi, channel, dbm_int2rfpwr(dbm),
						(const struct nrf24_mac*) &mac);
	if (err < 0)
		return err;

	l_timeout_create(5, beacon_timeout_cb, NULL, NULL);

	return 0;
}

void manager_stop(void)
{
	l_free(adapter.keys_pathname);
	l_queue_destroy(adapter.peer_offline_list, peer_free);
	l_queue_destroy(adapter.peer_online_list, peer_free);
	l_queue_destroy(adapter.beacon_list, beacon_free);
	radio_stop();
}
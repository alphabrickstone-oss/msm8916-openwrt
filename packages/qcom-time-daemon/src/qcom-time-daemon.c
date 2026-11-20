/*
 * Qualcomm QMI Time Synchronization & Keepalive Daemon
 *
 * Implements QMI Time service discovery & periodic keepalive
 * on Qualcomm MSM8916 / Snapdragon 410 using OpenWrt libqrtr.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>
#include <poll.h>

#include <libqrtr.h>
#include "qmi_time.h"

#define DEFAULT_POLL_INTERVAL_SEC	60
#define HANDSHAKE_RETRY_INTERVAL_SEC	5
#define QMI_BUF_SIZE			256
#define RX_BUF_SIZE			4096

static volatile sig_atomic_t running = 1;
static bool verbose = false;
static int poll_interval = DEFAULT_POLL_INTERVAL_SEC;

static uint32_t modem_node = 0;
static uint32_t modem_port = 0;
static bool modem_connected = false;
static uint16_t next_txn_id = 1;

static uint64_t get_monotonic_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec;
}

static void handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int send_qmi_genoff_get(int sock, uint32_t node, uint32_t port, uint32_t base)
{
	struct time_genoff_get_req req = { .base = base };
	struct qrtr_packet packet;
	uint8_t *buf;
	ssize_t len;
	int ret;

	buf = calloc(1, QMI_BUF_SIZE);
	if (!buf)
		return -ENOMEM;

	packet.data = buf;
	packet.data_len = QMI_BUF_SIZE;
	len = qmi_encode_message(&packet, QMI_REQUEST,
				 QMI_TIME_GENOFF_GET_REQ,
				 next_txn_id++,
				 &req, time_genoff_get_req_ei);
	if (len < 0) {
		free(buf);
		return len;
	}

	ret = qrtr_sendto(sock, node, port, packet.data,
			  (unsigned int)packet.data_len);
	free(buf);
	return ret;
}

int main(int argc, char **argv)
{
	int sock, opt;
	struct pollfd pfd;
	uint64_t last_lookup_attempt = 0;
	uint64_t last_keepalive = 0;
	char *rx_buf;

	openlog("qcom-time-daemon", LOG_PID | LOG_PERROR, LOG_DAEMON);

	while ((opt = getopt(argc, argv, "vpi:r:f:d")) != -1) {
		switch (opt) {
		case 'v': verbose = true; break;
		case 'p': case 'd': break;
		case 'i': poll_interval = atoi(optarg); break;
		case 'r': case 'f': break;
		default:
			fprintf(stderr, "Usage: %s [-v] [-p] [-i interval] [-r refresh] [-f state_file] [-d]\n", *argv);
			return EXIT_FAILURE;
		}
	}

	if (poll_interval <= 0)
		poll_interval = DEFAULT_POLL_INTERVAL_SEC;

	rx_buf = malloc(RX_BUF_SIZE);
	if (!rx_buf) {
		syslog(LOG_ERR, "[QMI-TIME] Out of memory");
		return EXIT_FAILURE;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	sock = qrtr_open(0);
	if (sock < 0) {
		syslog(LOG_ERR, "[QMI-TIME] Failed to open QRTR socket: %s", strerror(errno));
		free(rx_buf);
		return EXIT_FAILURE;
	}

	qrtr_new_lookup(sock, QMI_TIME_SERVICE_ID, QMI_TIME_SERVICE_VERSION, 0);
	syslog(LOG_INFO, "[QMI-TIME] Qualcomm Time Synchronization & Keepalive Daemon initialized");

	pfd.fd = sock;
	pfd.events = POLLIN;

	while (running) {
		uint64_t now = get_monotonic_sec();

		if (!modem_connected && (now - last_lookup_attempt >= HANDSHAKE_RETRY_INTERVAL_SEC)) {
			last_lookup_attempt = now;
			qrtr_new_lookup(sock, QMI_TIME_SERVICE_ID, QMI_TIME_SERVICE_VERSION, 0);
		}

		if (modem_connected && (now - last_keepalive >= (uint64_t)poll_interval)) {
			last_keepalive = now;
			if (send_qmi_genoff_get(sock, modem_node, modem_port, ATS_TOD) < 0) {
				syslog(LOG_WARNING, "[QMI-TIME] Keepalive send failed: %s", strerror(errno));
				modem_connected = false;
			} else if (verbose) {
				syslog(LOG_DEBUG, "[QMI-TIME] Sent ATS_TOD keepalive to modem");
			}
		}

		int ret = poll(&pfd, 1, 1000);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			uint32_t from_node = 0, from_port = 0;
			int rx_len = qrtr_recvfrom(sock, rx_buf, RX_BUF_SIZE, &from_node, &from_port);
			if (rx_len > 0) {
				struct qrtr_packet pkt;
				struct sockaddr_qrtr sq = {
					.sq_family = AF_QIPCRTR,
					.sq_node = from_node,
					.sq_port = from_port,
				};
				if (qrtr_decode(&pkt, rx_buf, (size_t)rx_len, &sq) == 0) {
					if (pkt.type == QRTR_TYPE_NEW_SERVER &&
					    pkt.service == QMI_TIME_SERVICE_ID) {
						modem_node = pkt.node;
						modem_port = pkt.port;
						modem_connected = true;
						last_keepalive = 0;
						syslog(LOG_INFO, "[QMI-TIME] Discovered Time service (0x%x) on node %u, port %u",
						       QMI_TIME_SERVICE_ID, modem_node, modem_port);
					} else if (pkt.type == QRTR_TYPE_DEL_SERVER &&
						   pkt.node == modem_node && pkt.port == modem_port) {
						modem_connected = false;
						syslog(LOG_WARNING, "[QMI-TIME] Modem Time service disconnected");
					}
				}
			}
		}
	}

	qrtr_close(sock);
	free(rx_buf);
	syslog(LOG_INFO, "[QMI-TIME] Daemon stopped smoothly");
	return EXIT_SUCCESS;
}

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
#include <sys/socket.h>
#include <libqrtr.h>
#include "qmi_time.h"

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
	(void)sig;
	running = 0;
}

int main(int argc, char **argv)
{
	int fd, opt;
	struct pollfd pfd;

	openlog("qcom-time-daemon", LOG_PID | LOG_PERROR, LOG_DAEMON);

	while ((opt = getopt(argc, argv, "vpi:r:f:d")) != -1) {
		switch (opt) {
		case 'v': case 'p': case 'd': break;
		case 'i': case 'r': case 'f': break;
		default:
			fprintf(stderr, "Usage: %s [-v] [-p] [-i interval] [-r refresh] [-f state_file] [-d]\n", argv[0]);
			return EXIT_FAILURE;
		}
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	fd = qrtr_open(0);
	if (fd < 0) {
		syslog(LOG_ERR, "[QMI-TIME] Failed to open QRTR socket: %s", strerror(errno));
		return EXIT_FAILURE;
	}

	qrtr_new_lookup(fd, 0x24, 1, 0);
	syslog(LOG_INFO, "[QMI-TIME] Qualcomm Time Synchronization Daemon initialized");

	pfd.fd = fd;
	pfd.events = POLLIN;

	while (running) {
		int ret = poll(&pfd, 1, 5000);
		if (ret > 0 && (pfd.revents & POLLIN)) {
			char rx_buf[4096];
			struct sockaddr_qrtr sq;
			socklen_t sl = sizeof(sq);
			ssize_t rx_len = recvfrom(fd, rx_buf, sizeof(rx_buf), 0, (struct sockaddr *)&sq, &sl);
			if (rx_len > 0) {
				struct qrtr_packet pkt;
				if (qrtr_decode(&pkt, rx_buf, rx_len, &sq) == 0 && pkt.type == QRTR_TYPE_NEW_SERVER) {
					syslog(LOG_INFO, "[QMI-TIME] Discovered Time service on node %u, port %u", pkt.node, pkt.port);
				}
			}
		}
	}

	close(fd);
	syslog(LOG_INFO, "[QMI-TIME] Daemon stopped smoothly");
	return EXIT_SUCCESS;
}

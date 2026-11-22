/* Host side of the companion app link, see app_bottom.h. */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "app_bottom.h"

static int server = -1, client = -1;

int app_bottom_listen(uint16_t port)
{
	int one = 1;
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
				   .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};

	server = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (server < 0) {
		perror("app: socket");
		return -1;
	}
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 1) < 0) {
		perror("app: bind");
		close(server);
		server = -1;
		return -1;
	}
	return 0;
}

int app_bottom_accept(void)
{
	int fd, one = 1;

	if (server < 0) {
		return 0;
	}
	fd = accept4(server, NULL, NULL, SOCK_NONBLOCK);
	if (fd < 0) {
		return 0;
	}
	if (client >= 0) {
		close(client); /* the new client replaces the old one */
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	client = fd;
	return 1;
}

int app_bottom_recv(uint8_t *data, size_t size)
{
	ssize_t n;

	if (client < 0) {
		return -1;
	}
	n = recv(client, data, size, 0);
	if (n > 0) {
		return (int)n;
	}
	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
		return 0;
	}
	close(client);
	client = -1;
	return -1;
}

int app_bottom_send(const uint8_t *data, size_t len)
{
	while (len > 0 && client >= 0) {
		ssize_t n = send(client, data, len, MSG_NOSIGNAL);

		if (n > 0) {
			data += n;
			len -= n;
		} else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			usleep(1000);
		} else {
			close(client);
			client = -1;
		}
	}
	return client >= 0 ? 0 : -1;
}

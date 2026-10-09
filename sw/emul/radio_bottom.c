/* Host side of the emulated radio, see radio_bottom.h. */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#include "radio_bottom.h"

#define GROUP "239.67.90.53" /* "CZ5" */

static int sock = -1;
static struct sockaddr_in group;

int radio_bottom_open(uint16_t port)
{
	int one = 1;
	unsigned char ttl = 1;
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port),
				   .sin_addr.s_addr = htonl(INADDR_ANY)};
	struct ip_mreq mreq;

	sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (sock < 0) {
		perror("radio: socket");
		return -1;
	}
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("radio: bind");
		goto fail;
	}
	/*
	 * Use the loopback interface: instances on this machine hear each other,
	 * independent of the network (multicast over Wi-Fi is often filtered).
	 */
	struct in_addr lo = {.s_addr = htonl(INADDR_LOOPBACK)};

	mreq.imr_multiaddr.s_addr = inet_addr(GROUP);
	mreq.imr_interface = lo;
	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		perror("radio: multicast join");
		goto fail;
	}
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &lo, sizeof(lo));
	/* Instances on the same machine hear each other. */
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one));
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	group = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port),
				     .sin_addr.s_addr = inet_addr(GROUP)};
	return 0;
fail:
	close(sock);
	sock = -1;
	return -1;
}

int radio_bottom_send(const uint8_t *data, size_t len)
{
	if (sock < 0) {
		return -1;
	}
	return sendto(sock, data, len, 0, (struct sockaddr *)&group, sizeof(group)) == (ssize_t)len
		       ? 0
		       : -1;
}

int radio_bottom_recv(uint8_t *data, size_t size)
{
	if (sock < 0) {
		return 0;
	}
	ssize_t n = recv(sock, data, size, 0);

	return n > 0 ? (int)n : 0;
}

void radio_bottom_random(void *buf, size_t len)
{
	if (getrandom(buf, len, 0) != (ssize_t)len) {
		memset(buf, 0x5a, len);
	}
}

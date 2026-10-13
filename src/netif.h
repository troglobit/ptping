#ifndef TAPING_NETIF_H
#define TAPING_NETIF_H

#include <net/if.h>
#include <stdint.h>
#include <sys/socket.h>

struct netif {
	unsigned ifindex;
	char name[IF_NAMESIZE];
	uint8_t mac[6];
};

/* The interface that owns the local address sa. Returns 0, or -1
 * with errno set (ENODEV if no interface has it). */
int netif_from_local(const struct sockaddr *sa, struct netif *ni);

/* The interface the kernel would send to the remote address sa
 * through, found by connecting a throwaway UDP socket to it. */
int netif_to_remote(const struct sockaddr *sa, socklen_t len, struct netif *ni);

#endif

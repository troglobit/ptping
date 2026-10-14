#ifndef PTPING_NETIF_H
#define PTPING_NETIF_H

#include <net/if.h>
#include <netinet/in.h>
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

/* Resolve a host name or numeric address, with port. Returns 0, or
 * -1 having logged why. */
int addr_resolve(const char *host, uint16_t port, struct sockaddr_storage *ss,
		 socklen_t *len);

void addr_set_port(struct sockaddr_storage *ss, uint16_t port);

/* Set the DSCP of everything sent on a UDP socket of the given family. */
int sock_set_dscp(int fd, int family, uint8_t dscp);

/* Numeric form of an address, for messages. buf must hold ADDR_STRLEN. */
#define ADDR_STRLEN (INET6_ADDRSTRLEN + IF_NAMESIZE + 8)
const char *addr_str(const struct sockaddr *sa, char *buf);

#endif

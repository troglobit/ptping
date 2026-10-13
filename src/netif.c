#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "netif.h"

static int sa_equal(const struct sockaddr *a, const struct sockaddr *b)
{
	if (a->sa_family != b->sa_family)
		return 0;

	switch (a->sa_family) {
	case AF_INET:
		return !memcmp(&((struct sockaddr_in *)a)->sin_addr,
			       &((struct sockaddr_in *)b)->sin_addr,
			       sizeof(struct in_addr));
	case AF_INET6:
		return !memcmp(&((struct sockaddr_in6 *)a)->sin6_addr,
			       &((struct sockaddr_in6 *)b)->sin6_addr,
			       sizeof(struct in6_addr));
	}

	return 0;
}

/* Fill ni from the AF_PACKET entry of the named interface, which
 * carries both its index and its MAC address. */
static int netif_from_name(struct ifaddrs *ifa, const char *name, struct netif *ni)
{
	const struct sockaddr_ll *sll;
	struct ifaddrs *i;

	for (i = ifa; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET ||
		    strcmp(i->ifa_name, name))
			continue;

		sll = (const struct sockaddr_ll *)i->ifa_addr;
		ni->ifindex = sll->sll_ifindex;
		memcpy(ni->mac, sll->sll_addr, sizeof(ni->mac));
		snprintf(ni->name, sizeof(ni->name), "%s", name);
		return 0;
	}

	errno = ENODEV;
	return -1;
}

int netif_from_local(const struct sockaddr *sa, struct netif *ni)
{
	const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
	char scoped[IF_NAMESIZE];
	const char *name = NULL;
	struct ifaddrs *ifa, *i;
	int ret;

	if (sa->sa_family == AF_INET6 && sin6->sin6_scope_id)
		name = if_indextoname(sin6->sin6_scope_id, scoped);

	if (getifaddrs(&ifa))
		return -1;

	for (i = ifa; !name && i; i = i->ifa_next) {
		if (i->ifa_addr && sa_equal(i->ifa_addr, sa))
			name = i->ifa_name;
	}

	if (name) {
		ret = netif_from_name(ifa, name, ni);
	} else {
		errno = ENODEV;
		ret = -1;
	}

	freeifaddrs(ifa);
	return ret;
}

int netif_to_remote(const struct sockaddr *sa, socklen_t len, struct netif *ni)
{
	struct sockaddr_storage local;
	socklen_t llen = sizeof(local);
	int fd, ret = -1;

	fd = socket(sa->sa_family, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	if (!connect(fd, sa, len) &&
	    !getsockname(fd, (struct sockaddr *)&local, &llen))
		ret = netif_from_local((struct sockaddr *)&local, ni);

	close(fd);
	return ret;
}

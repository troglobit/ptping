#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#include "netif.h"
#include "test.h"

int main(void)
{
	struct sockaddr_in sin = { .sin_family = AF_INET };
	struct sockaddr_in6 sin6 = { .sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT };
	struct netif ni;

	check(inet_pton(AF_INET, "127.0.0.1", &sin.sin_addr) == 1);
	check(!netif_from_local((struct sockaddr *)&sin, &ni));
	check(!strcmp(ni.name, "lo"));
	check(ni.ifindex == if_nametoindex("lo"));

	memset(&ni, 0, sizeof(ni));
	check(!netif_to_remote((struct sockaddr *)&sin, sizeof(sin), &ni));
	check(!strcmp(ni.name, "lo"));

	memset(&ni, 0, sizeof(ni));
	check(!netif_from_local((struct sockaddr *)&sin6, &ni));
	check(!strcmp(ni.name, "lo"));

	/* A scope id is authoritative. */
	sin6.sin6_scope_id = if_nametoindex("lo");
	memset(&sin6.sin6_addr, 0, sizeof(sin6.sin6_addr));
	check(!netif_from_local((struct sockaddr *)&sin6, &ni));
	check(!strcmp(ni.name, "lo"));

	/* An address nobody has. */
	check(inet_pton(AF_INET, "192.0.2.1", &sin.sin_addr) == 1);
	check(netif_from_local((struct sockaddr *)&sin, &ni) == -1);

	return 0;
}

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "ts.h"
#include "tspec.h"

static void test_netif(void)
{
	/* lo has no PHC. */
	check(ts_setup_netif("lo", TS_MODE_SW) == TS_MODE_SW);
	check(ts_setup_netif("lo", TS_MODE_ANY) == TS_MODE_SW);
	check(ts_setup_netif("lo", TS_MODE_HW) == -1);
	check(ts_setup_netif("nonexistent0", TS_MODE_HW) == -1);
}

static void test_sock(void)
{
	struct sockaddr_in sin = {
		.sin_family = AF_INET,
		.sin_port = htons(31901),
		.sin_addr.s_addr = htonl(INADDR_LOOPBACK),
	};
	char buf[64], cbuf[256];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	struct pollfd pfd = { .events = POLLERR };
	int64_t before, ts, tx[2];
	struct ts_pair rx_ts;
	uint32_t id;
	int rx, fd, i;

	rx = socket(AF_INET, SOCK_DGRAM, 0);
	check(rx >= 0 && !bind(rx, (struct sockaddr *)&sin, sizeof(sin)));
	check(!ts_setup_sock(rx, TS_MODE_SW));

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	check(fd >= 0 && !ts_setup_sock(fd, TS_MODE_SW));

	/* The kernel turns on reception timestamping from a work item,
	 * so give it a moment before sending. */
	usleep(50000);

	/* Nothing queued yet. */
	check(ts_recv_tx(fd, TS_MODE_SW, &id, &ts) == 0);

	before = ts_now(CLOCK_REALTIME);
	for (i = 0; i < 2; i++)
		check(sendto(fd, "ab", 2, 0, (struct sockaddr *)&sin, sizeof(sin)) == 2);

	/* Ids count from 0 in send order. */
	pfd.fd = fd;
	for (i = 0; i < 2; i++) {
		check(poll(&pfd, 1, 1000) == 1);
		check(ts_recv_tx(fd, TS_MODE_SW, &id, &tx[i]) == 1);
		check(id == (uint32_t)i);
		check(tx[i] >= before && tx[i] <= ts_now(CLOCK_REALTIME));
	}
	check(tx[1] >= tx[0]);
	check(ts_recv_tx(fd, TS_MODE_SW, &id, &ts) == 0);

	/* Reception timestamps. */
	for (i = 0; i < 2; i++) {
		msg.msg_controllen = sizeof(cbuf);
		check(recvmsg(rx, &msg, 0) == 2);
		ts_from_msghdr(&msg, &rx_ts);
		check(rx_ts.hw == TS_NONE);
		ts = ts_pick(&rx_ts, TS_MODE_SW);
		check(ts >= tx[i] && ts <= ts_now(CLOCK_REALTIME));
	}

	close(fd);
	close(rx);
}

int main(void)
{
	test_netif();
	test_sock();
	return 0;
}

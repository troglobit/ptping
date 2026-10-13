#ifndef TAPING_TS_H
#define TAPING_TS_H

#include <stdint.h>
#include <sys/socket.h>

enum ts_mode {
	TS_MODE_HW,
	TS_MODE_SW,
	TS_MODE_ANY,		/* hw if possible, otherwise sw */
};

const char *ts_mode_str(enum ts_mode mode);

/* Returns the mode named by str, or -1. */
int ts_mode_parse(const char *str);

/* Make sure the interface delivers the timestamps mode asks for,
 * enabling hardware timestamping of PTP messages if it is not already
 * on. Returns the mode in effect, TS_MODE_HW or TS_MODE_SW, or -1
 * with errno set if mode is TS_MODE_HW and the interface cannot. */
int ts_setup_netif(const char *ifname, enum ts_mode mode);

/* Ask for reception and transmission timestamps of the given mode on
 * the socket, which must be TS_MODE_HW or TS_MODE_SW. Transmission
 * timestamps come back on the error queue with a counter that starts
 * at 0 and increments per packet sent on the socket. */
int ts_setup_sock(int fd, enum ts_mode mode);

/* The reception timestamp in the control data of a received message,
 * or TS_NONE. */
int64_t ts_from_msghdr(const struct msghdr *msg);

/* Take one transmission timestamp off the error queue. Returns 1 with
 * *id and *ts filled in, 0 if the queue is empty, or -1 on error. */
int ts_recv_tx(int fd, uint32_t *id, int64_t *ts);

#endif

#include <errno.h>
#include <time.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "log.h"
#include "ts.h"
#include "tspec.h"

static const char *const ts_mode_names[] = {
	[TS_MODE_HW] = "hw",
	[TS_MODE_SW] = "sw",
	[TS_MODE_ANY] = "any",
};

const char *ts_mode_str(enum ts_mode mode)
{
	return ts_mode_names[mode];
}

int ts_mode_parse(const char *str)
{
	unsigned i;

	for (i = 0; i < sizeof(ts_mode_names) / sizeof(ts_mode_names[0]); i++) {
		if (!strcmp(str, ts_mode_names[i]))
			return i;
	}

	return -1;
}

static int hwtstamp_ioctl(int fd, const char *ifname, unsigned long req,
			  struct hwtstamp_config *cfg)
{
	struct ifreq ifr = { .ifr_data = (char *)cfg };

	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
	return ioctl(fd, req, &ifr);
}

static int rx_filter_covers_sync(int filter)
{
	switch (filter) {
	case HWTSTAMP_FILTER_ALL:
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		return 1;
	}

	return 0;
}

/* Returns 0 if hardware timestamping of Sync messages is on, -1
 * otherwise. Leaves an already suitable configuration alone, since
 * it is most likely ptp4l's. */
static int hwtstamp_enable(const char *ifname)
{
	static const int filters[] = {
		HWTSTAMP_FILTER_PTP_V2_EVENT,
		HWTSTAMP_FILTER_PTP_V2_L4_EVENT,
		HWTSTAMP_FILTER_ALL,
	};
	struct hwtstamp_config cfg = { 0 };
	int fd, ret = -1;
	unsigned i;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -1;

	if (!hwtstamp_ioctl(fd, ifname, SIOCGHWTSTAMP, &cfg) &&
	    cfg.tx_type != HWTSTAMP_TX_OFF && rx_filter_covers_sync(cfg.rx_filter)) {
		dbg("%s: hardware timestamping already enabled", ifname);
		ret = 0;
		goto out;
	}

	for (i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
		cfg = (struct hwtstamp_config){
			.tx_type = HWTSTAMP_TX_ON,
			.rx_filter = filters[i],
		};
		if (!hwtstamp_ioctl(fd, ifname, SIOCSHWTSTAMP, &cfg)) {
			dbg("%s: enabled hardware timestamping, rx filter %d",
			    ifname, cfg.rx_filter);
			ret = 0;
			goto out;
		}
	}

	dbg("%s: no hardware timestamping: %m", ifname);
out:
	close(fd);
	return ret;
}

int ts_setup_netif(const char *ifname, enum ts_mode mode)
{
	if (mode == TS_MODE_SW)
		return TS_MODE_SW;

	if (!hwtstamp_enable(ifname))
		return TS_MODE_HW;

	if (mode == TS_MODE_HW)
		return -1;

	warn("%s: no hardware timestamping, using software timestamps", ifname);
	return TS_MODE_SW;
}

int ts_setup_sock(int fd, enum ts_mode mode)
{
	int flags = SOF_TIMESTAMPING_OPT_CMSG | SOF_TIMESTAMPING_OPT_ID |
		SOF_TIMESTAMPING_OPT_TSONLY;

	if (mode != TS_MODE_SW)
		flags |= SOF_TIMESTAMPING_RX_HARDWARE | SOF_TIMESTAMPING_TX_HARDWARE |
			SOF_TIMESTAMPING_RAW_HARDWARE;
	if (mode != TS_MODE_HW)
		flags |= SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_TX_SOFTWARE |
			SOF_TIMESTAMPING_SOFTWARE;

	return setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
}

static int64_t ts_from_slot(const struct timespec *ts)
{
	return (ts->tv_sec || ts->tv_nsec) ? ts_from_timespec(ts) : TS_NONE;
}

void ts_from_msghdr(const struct msghdr *msg, struct ts_pair *ts)
{
	const struct scm_timestamping *tss;
	struct cmsghdr *cm;

	ts->hw = ts->sw = TS_NONE;

	for (cm = CMSG_FIRSTHDR(msg); cm; cm = CMSG_NXTHDR((struct msghdr *)msg, cm)) {
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_TIMESTAMPING)
			continue;

		tss = (const struct scm_timestamping *)CMSG_DATA(cm);
		ts->sw = ts_from_slot(&tss->ts[0]);
		ts->hw = ts_from_slot(&tss->ts[2]);	/* raw hardware */
		return;
	}
}

int ts_setup_txtime(int fd)
{
	struct sock_txtime cfg = {
		.clockid = CLOCK_TAI,
		.flags = SOF_TXTIME_REPORT_ERRORS,
	};

	return setsockopt(fd, SOL_SOCKET, SO_TXTIME, &cfg, sizeof(cfg));
}

ssize_t ts_sendto(int fd, const void *buf, size_t len,
		  const struct sockaddr *sa, socklen_t salen, int64_t txtime)
{
	struct iovec iov = { (void *)buf, len };
	char cbuf[CMSG_SPACE(sizeof(uint64_t))];
	struct msghdr msg = {
		.msg_name = (void *)sa,
		.msg_namelen = salen,
		.msg_iov = &iov,
		.msg_iovlen = 1,
	};
	struct cmsghdr *cm;

	if (txtime == TS_NONE)
		return sendto(fd, buf, len, 0, sa, salen);

	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);
	cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_TXTIME;
	cm->cmsg_len = CMSG_LEN(sizeof(uint64_t));
	memcpy(CMSG_DATA(cm), &txtime, sizeof(uint64_t));
	return sendmsg(fd, &msg, 0);
}

int ts_recv_tx(int fd, enum ts_mode mode, uint32_t *id, int64_t *ts)
{
	const struct sock_extended_err *ee = NULL;
	struct ts_pair pair;
	struct cmsghdr *cm;
	char cbuf[256];
	struct msghdr msg = {
		.msg_control = cbuf,
		.msg_controllen = sizeof(cbuf),
	};

	if (recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT) < 0)
		return (errno == EAGAIN || errno == EINTR) ? 0 : -1;

	for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
		if ((cm->cmsg_level == SOL_IP && cm->cmsg_type == IP_RECVERR) ||
		    (cm->cmsg_level == SOL_IPV6 && cm->cmsg_type == IPV6_RECVERR))
			ee = (const struct sock_extended_err *)CMSG_DATA(cm);
	}

	if (ee && ee->ee_origin == SO_EE_ORIGIN_TXTIME) {
		dbg("a probe missed its launch time and was dropped");
		return 0;
	}
	if (!ee || ee->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
		dbg("ignoring error queue entry that is not a timestamp");
		return 0;
	}

	ts_from_msghdr(&msg, &pair);
	*id = ee->ee_data;
	*ts = ts_pick(&pair, mode);
	return 1;
}

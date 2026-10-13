#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "be.h"
#include "ptp.h"
#include "ts.h"

#define PTP_VERSION 2
#define PTP_MSG_SYNC 0

/* flagField, first octet: twoStepFlag and unicastFlag */
#define PTP_FLAGS_SYNC 0x0600

#define PTP_TLV_PAD 0x8008
#define PTP_TLV_HDR_LEN 4

/* Field offsets, IEEE 1588-2019 table 35 and 44. */
enum {
	OFF_TYPE      = 0,
	OFF_VERSION   = 1,
	OFF_LENGTH    = 2,
	OFF_DOMAIN    = 4,
	OFF_FLAGS     = 6,
	OFF_CORR      = 8,
	OFF_SRC_PORT  = 20,
	OFF_SEQ       = 30,
	OFF_LOG_IVAL  = 33,
};

int ptp_sync_build(uint8_t *buf, size_t len, const struct ptp_port_id *src,
		   uint8_t domain, uint16_t seq)
{
	if (len < PTP_SYNC_LEN)
		return -1;
	if (len != PTP_SYNC_LEN && (len < PTP_SYNC_LEN + PTP_TLV_HDR_LEN || len & 1))
		return -1;

	memset(buf, 0, len);
	buf[OFF_TYPE] = PTP_MSG_SYNC;
	buf[OFF_VERSION] = PTP_VERSION;
	put16(buf + OFF_LENGTH, len);
	buf[OFF_DOMAIN] = domain;
	put16(buf + OFF_FLAGS, PTP_FLAGS_SYNC);
	ptp_port_id_put(buf + OFF_SRC_PORT, src);
	put16(buf + OFF_SEQ, seq);
	buf[OFF_LOG_IVAL] = 0x7f;

	if (len > PTP_SYNC_LEN) {
		put16(buf + PTP_SYNC_LEN, PTP_TLV_PAD);
		put16(buf + PTP_SYNC_LEN + 2, len - PTP_SYNC_LEN - PTP_TLV_HDR_LEN);
	}

	return 0;
}

void ptp_sync_set_seq(uint8_t *buf, uint16_t seq)
{
	put16(buf + OFF_SEQ, seq);
}

int ptp_sync_parse(const uint8_t *buf, size_t len, struct ptp_sync *sync)
{
	if (len < PTP_SYNC_LEN)
		return -1;
	if ((buf[OFF_TYPE] & 0x0f) != PTP_MSG_SYNC)
		return -1;
	if ((buf[OFF_VERSION] & 0x0f) != PTP_VERSION)
		return -1;
	if (get16(buf + OFF_LENGTH) > len)
		return -1;

	sync->domain = buf[OFF_DOMAIN];
	ptp_port_id_get(buf + OFF_SRC_PORT, &sync->src);
	sync->seq = get16(buf + OFF_SEQ);
	sync->correction = (int64_t)get64(buf + OFF_CORR);
	return 0;
}

uint64_t ptp_seq_extend(uint64_t last, uint16_t seq)
{
	return last + (int16_t)(seq - (uint16_t)last);
}

void ptp_port_id_from_mac(struct ptp_port_id *pid, const uint8_t mac[6],
			  uint16_t port)
{
	memcpy(pid->clock_id, mac, 3);
	pid->clock_id[3] = 0xff;
	pid->clock_id[4] = 0xfe;
	memcpy(pid->clock_id + 5, mac + 3, 3);
	pid->port = port;
}

void ptp_port_id_put(uint8_t *buf, const struct ptp_port_id *pid)
{
	memcpy(buf, pid->clock_id, sizeof(pid->clock_id));
	put16(buf + 8, pid->port);
}

void ptp_port_id_get(const uint8_t *buf, struct ptp_port_id *pid)
{
	memcpy(pid->clock_id, buf, sizeof(pid->clock_id));
	pid->port = get16(buf + 8);
}

void ptp_port_id_self(struct ptp_port_id *pid, const uint8_t mac[6])
{
	ptp_port_id_from_mac(pid, mac, getpid() % 65535 + 1);
}

int ptp_port_id_cmp(const struct ptp_port_id *a, const struct ptp_port_id *b)
{
	int c = memcmp(a->clock_id, b->clock_id, sizeof(a->clock_id));

	return c ? c : (int)a->port - (int)b->port;
}

const char *ptp_port_id_str(const struct ptp_port_id *pid, char *buf)
{
	const uint8_t *c = pid->clock_id;

	snprintf(buf, PTP_PORT_ID_STRLEN, "%02x%02x%02x%02x%02x%02x%02x%02x.%u",
		 c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], pid->port);
	return buf;
}

int ptp_recv_sync(int fd, struct ptp_sync *sync, int64_t *rx_ts)
{
	uint8_t buf[2048], cbuf[256];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf,
	};
	ssize_t n;

	for (;;) {
		msg.msg_controllen = sizeof(cbuf);
		n = recvmsg(fd, &msg, MSG_DONTWAIT);
		if (n < 0)
			return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
		if (ptp_sync_parse(buf, n, sync))
			continue;

		*rx_ts = ts_from_msghdr(&msg);
		return 1;
	}
}

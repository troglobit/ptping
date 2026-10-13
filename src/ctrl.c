#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "be.h"
#include "ctrl.h"

#define HELLO_LEN 30
#define WELCOME_FIXED_LEN 14

static uint8_t *put_text(uint8_t *p, const char *text)
{
	size_t n = strnlen(text, CTRL_TEXT_MAX);

	memcpy(p, text, n);
	return p + n;
}

static void get_text(const uint8_t *p, size_t n, char *text)
{
	memcpy(text, p, n);
	text[n] = '\0';
}

size_t ctrl_encode(const struct ctrl_msg *msg, uint8_t *buf)
{
	uint8_t *p = buf + CTRL_HDR_LEN;
	unsigned i;

	switch (msg->type) {
	case CTRL_HELLO:
		*p++ = CTRL_VERSION;
		*p++ = msg->hello.flags;
		*p++ = msg->hello.domain;
		*p++ = msg->hello.tstamp;
		ptp_port_id_put(p, &msg->hello.src);
		p += PTP_PORT_ID_LEN;
		put64(p, msg->hello.rev.period_ns);
		put32(p + 8, msg->hello.rev.count);
		put16(p + 12, msg->hello.rev.size);
		p[14] = msg->hello.rev.dscp;
		p[15] = 0;
		p += 16;
		break;
	case CTRL_WELCOME:
		*p++ = CTRL_VERSION;
		*p++ = msg->welcome.status;
		*p++ = msg->welcome.tstamp;
		*p++ = 0;
		ptp_port_id_put(p, &msg->welcome.src);
		p = put_text(p + PTP_PORT_ID_LEN, msg->welcome.text);
		break;
	case CTRL_TS:
		*p++ = msg->ts.flow;
		*p++ = msg->ts.kind;
		put16(p, msg->ts.count);
		p += 2;
		for (i = 0; i < msg->ts.count; i++, p += CTRL_TS_REC_LEN) {
			put64(p, msg->ts.rec[i].seq);
			put64(p + 8, msg->ts.rec[i].ts);
			put64(p + 16, msg->ts.rec[i].correction);
		}
		break;
	case CTRL_BYE:
		break;
	case CTRL_ERROR:
		p = put_text(p, msg->text);
		break;
	}

	buf[0] = msg->type;
	buf[1] = 0;
	put16(buf + 2, p - buf - CTRL_HDR_LEN);
	return p - buf;
}

ssize_t ctrl_decode(const uint8_t *buf, size_t len, struct ctrl_msg *msg)
{
	const uint8_t *p = buf + CTRL_HDR_LEN;
	size_t plen;
	unsigned i;

	if (len < CTRL_HDR_LEN)
		return 0;

	plen = get16(buf + 2);
	if (plen > CTRL_MSG_MAX - CTRL_HDR_LEN)
		return -1;
	if (len < CTRL_HDR_LEN + plen)
		return 0;

	memset(msg, 0, sizeof(*msg));
	msg->type = buf[0];

	switch (msg->type) {
	case CTRL_HELLO:
		if (plen != HELLO_LEN || p[0] != CTRL_VERSION)
			return -1;
		msg->hello.flags = p[1];
		msg->hello.domain = p[2];
		msg->hello.tstamp = p[3];
		ptp_port_id_get(p + 4, &msg->hello.src);
		p += 4 + PTP_PORT_ID_LEN;
		msg->hello.rev.period_ns = get64(p);
		msg->hello.rev.count = get32(p + 8);
		msg->hello.rev.size = get16(p + 12);
		msg->hello.rev.dscp = p[14];
		break;
	case CTRL_WELCOME:
		if (plen < WELCOME_FIXED_LEN ||
		    plen > WELCOME_FIXED_LEN + CTRL_TEXT_MAX || p[0] != CTRL_VERSION)
			return -1;
		msg->welcome.status = p[1];
		msg->welcome.tstamp = p[2];
		ptp_port_id_get(p + 4, &msg->welcome.src);
		get_text(p + 4 + PTP_PORT_ID_LEN, plen - WELCOME_FIXED_LEN,
			 msg->welcome.text);
		break;
	case CTRL_TS:
		if (plen < 4)
			return -1;
		msg->ts.flow = p[0];
		msg->ts.kind = p[1];
		msg->ts.count = get16(p + 2);
		if (msg->ts.count > CTRL_TS_BATCH ||
		    plen != 4 + (size_t)msg->ts.count * CTRL_TS_REC_LEN)
			return -1;
		p += 4;
		for (i = 0; i < msg->ts.count; i++, p += CTRL_TS_REC_LEN) {
			msg->ts.rec[i].seq = get64(p);
			msg->ts.rec[i].ts = get64(p + 8);
			msg->ts.rec[i].correction = get64(p + 16);
		}
		break;
	case CTRL_BYE:
		if (plen)
			return -1;
		break;
	case CTRL_ERROR:
		if (plen > CTRL_TEXT_MAX)
			return -1;
		get_text(p, plen, msg->text);
		break;
	default:
		return -1;
	}

	return CTRL_HDR_LEN + plen;
}

void ctrl_conn_init(struct ctrl_conn *c, int fd)
{
	int one = 1, idle = 2, intvl = 1, cnt = 3;

	c->fd = fd;
	c->len = 0;

	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

int ctrl_send(struct ctrl_conn *c, const struct ctrl_msg *msg)
{
	uint8_t buf[CTRL_MSG_MAX];
	size_t len = ctrl_encode(msg, buf), off = 0;
	ssize_t n;

	while (off < len) {
		n = send(c->fd, buf + off, len - off, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += n;
	}

	return 0;
}

int ctrl_recv(struct ctrl_conn *c, struct ctrl_msg *msg)
{
	ssize_t n;

	for (;;) {
		n = ctrl_decode(c->buf, c->len, msg);
		if (n > 0) {
			c->len -= n;
			memmove(c->buf, c->buf + n, c->len);
			return 1;
		}
		if (n < 0)
			break;

		n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len,
			 MSG_DONTWAIT);
		if (n < 0)
			return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
		if (n == 0)
			break;

		c->len += n;
	}

	errno = 0;
	return -1;
}

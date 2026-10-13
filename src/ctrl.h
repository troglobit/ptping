#ifndef TAPING_CTRL_H
#define TAPING_CTRL_H

#include <stddef.h>
#include <stdint.h>

#include "ptp.h"

/* The control protocol runs over TCP from source to sink. Every
 * message is a four byte header (type, flags, 16-bit payload length)
 * followed by the payload; all integers big-endian. */

enum ctrl_type {
	CTRL_HELLO = 1,		/* source to sink: open a session */
	CTRL_WELCOME,		/* sink to source: accept or refuse */
	CTRL_TS,		/* sink to source: batch of timestamps */
	CTRL_BYE,		/* either: orderly close */
	CTRL_ERROR,		/* either: fatal problem, text follows */
};

#define CTRL_VERSION 1
#define CTRL_HELLO_BIDIR 0x01

enum ctrl_tstamp {
	CTRL_TS_HW,
	CTRL_TS_SW,
	CTRL_TS_ANY,
};

enum ctrl_flow {
	CTRL_FLOW_FWD,		/* source to sink */
	CTRL_FLOW_REV,		/* sink to source */
};

enum ctrl_kind {
	CTRL_KIND_RX,
	CTRL_KIND_TX,
};

/* What a sender needs to know to run a flow. */
struct ctrl_flow_params {
	uint64_t period_ns;
	uint32_t count;		/* 0: until BYE */
	uint16_t size;		/* PTP message length */
	uint8_t dscp;
};

struct ctrl_hello {
	uint8_t flags;
	uint8_t domain;
	uint8_t tstamp;		/* enum ctrl_tstamp */
	struct ptp_port_id src;
	struct ctrl_flow_params rev;	/* only read when flags has BIDIR */
};

#define CTRL_TEXT_MAX 255

struct ctrl_welcome {
	uint8_t status;		/* 0: ok, otherwise refused, see text */
	uint8_t tstamp;		/* CTRL_TS_HW or CTRL_TS_SW, in use at sink */
	struct ptp_port_id src;	/* sink's identity on reverse probes */
	char text[CTRL_TEXT_MAX + 1];
};

struct ctrl_ts_rec {
	uint64_t seq;		/* extended sequence number */
	int64_t ts;		/* ns */
	int64_t correction;	/* raw correctionField, 0 for tx */
};

#define CTRL_TS_BATCH 64

struct ctrl_ts {
	uint8_t flow;		/* enum ctrl_flow */
	uint8_t kind;		/* enum ctrl_kind */
	uint16_t count;
	struct ctrl_ts_rec rec[CTRL_TS_BATCH];
};

struct ctrl_msg {
	enum ctrl_type type;
	union {
		struct ctrl_hello hello;
		struct ctrl_welcome welcome;
		struct ctrl_ts ts;
		char text[CTRL_TEXT_MAX + 1];	/* ERROR */
	};
};

#define CTRL_HDR_LEN 4
#define CTRL_TS_REC_LEN 24

/* Large enough for any single message. */
#define CTRL_MSG_MAX (CTRL_HDR_LEN + 4 + CTRL_TS_BATCH * CTRL_TS_REC_LEN)

/* Returns the number of bytes written to buf, which must hold
 * CTRL_MSG_MAX. */
size_t ctrl_encode(const struct ctrl_msg *msg, uint8_t *buf);

/* Returns the number of bytes consumed from buf, 0 if a complete
 * message is not yet there, or -1 if the data is not a valid message. */
ssize_t ctrl_decode(const uint8_t *buf, size_t len, struct ctrl_msg *msg);

/* A connection with a receive buffer. */
struct ctrl_conn {
	int fd;
	uint8_t buf[2 * CTRL_MSG_MAX];
	size_t len;
};

/* Takes over fd, and on a TCP socket turns on keepalive with a short
 * interval, so a dead peer is noticed within seconds. */
void ctrl_conn_init(struct ctrl_conn *c, int fd);

/* Write a whole message, blocking. Returns 0 or -1 with errno set. */
int ctrl_send(struct ctrl_conn *c, const struct ctrl_msg *msg);

/* Read whatever the socket has without blocking, and decode one
 * message if a whole one is buffered. Returns 1 if msg was filled,
 * 0 if not yet, or -1 on EOF, a socket error or a malformed stream
 * (errno is 0 for EOF and malformed data). Call again on 1, since
 * more than one message may be buffered. */
int ctrl_recv(struct ctrl_conn *c, struct ctrl_msg *msg);

#endif

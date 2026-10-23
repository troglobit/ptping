#ifndef PTPING_SOURCE_H
#define PTPING_SOURCE_H

#include <sys/socket.h>

#include "ctrl.h"
#include "flow.h"
#include "netif.h"
#include "ptp.h"
#include "ptping.h"
#include "sched.h"

/* The source runs streams of probes, one session with a sink each. It
 * sends the probes, collects timestamps from both ends, pairs them
 * into samples, and hands the samples to a consumer: probe, stat or
 * record. */

struct source;
struct stream;

struct source_ops {
	/* A resolved sample. May be NULL. */
	void (*sample)(void *priv, const struct stream *st, enum ctrl_flow flow,
		       const struct flow_sample *s);
	/* Every report interval, and once more at the end. May be NULL. */
	void (*report)(void *priv, const struct source *src, int final);
};

struct stream_flow {
	struct stream *st;
	enum ctrl_flow which;
	struct flow flow;
};

/* A socket on the probe port of one local address, which takes the
 * reverse probes of every stream from that address. */
struct source_rx {
	struct sockaddr_storage addr;
	int fd;
};

struct stream {
	struct source *src;
	const struct stream_opts *opts;
	unsigned index;		/* in src->streams */

	enum ts_mode mode;	/* in effect at both ends */
	struct netif ni;
	struct stream_flow flow[2];	/* indexed by enum ctrl_flow */
	struct sched sched;	/* of the forward flow; the reverse one matches */

	/* Private to source.c */
	struct sockaddr_storage sink;
	socklen_t sink_len;
	struct ctrl_conn ctrl;
	struct ptp_port_id self, sink_id;
	int tx_fd;
	uint8_t *pkt;
	size_t pkt_len;
	uint64_t seq;		/* next forward probe */
	uint64_t tx_seq;	/* extension state for the tx counter */
	uint64_t rev_seq;	/* extension state for reverse sequenceId */
	int64_t stop_at;	/* drain deadline once sending is over; 0 while sending */
	int lost;		/* the sink went away */
};

struct source {
	struct opts *opts;
	const struct source_ops *ops;
	void *priv;

	struct stream *streams;	/* one per opts->streams */
	int64_t started;	/* CLOCK_MONOTONIC */

	/* Private to source.c */
	struct source_rx rx[PTP_SELF_PORTS];
	unsigned nrx;
	int report_timer;
};

/* Run a stream to every sink in opts until each has reached its
 * count, its sink has gone away, or the user interrupts. Returns 0, or
 * 1 if a session could not be set up or was lost. */
int source_run(struct opts *opts, const struct source_ops *ops, void *priv);

/* Where in the cycle a timestamp of this stream falls, and which
 * cycle; TS_NONE without a window or a timestamp. */
static inline int64_t stream_phase(const struct stream *st, int64_t ts)
{
	return sched_phase(&st->sched, ts, st->mode);
}

static inline int64_t stream_cycle(const struct stream *st, int64_t ts)
{
	return sched_cycle(&st->sched, ts, st->mode);
}

#endif

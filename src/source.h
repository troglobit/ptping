#ifndef PTPING_SOURCE_H
#define PTPING_SOURCE_H

#include <sys/socket.h>

#include "ctrl.h"
#include "flow.h"
#include "netif.h"
#include "ptping.h"
#include "sched.h"

/* The source sends probes, collects timestamps from both ends, pairs
 * them into samples, and hands the samples to a consumer: probe, stat
 * or record. */

struct source;

struct source_ops {
	/* A resolved sample. May be NULL. */
	void (*sample)(void *priv, const struct source *src, enum ctrl_flow flow,
		       const struct flow_sample *s);
	/* Every report interval, and once more at the end. May be NULL. */
	void (*report)(void *priv, const struct source *src, int final);
};

struct source_flow {
	struct source *src;
	enum ctrl_flow which;
	struct flow flow;
};

struct source {
	struct opts *opts;
	const struct source_ops *ops;
	void *priv;

	enum ts_mode mode;	/* in effect at both ends */
	struct netif ni;
	struct source_flow flow[2];	/* indexed by enum ctrl_flow */
	struct sched sched;	/* of the forward flow; the reverse one matches */
	int64_t started;	/* CLOCK_MONOTONIC */

	/* Private to source.c */
	struct sockaddr_storage sink;
	socklen_t sink_len;
	struct ctrl_conn ctrl;
	struct ptp_port_id self, sink_id;
	int probe_fd, report_timer;
	uint8_t *pkt;
	size_t pkt_len;
	uint64_t seq;		/* next forward probe */
	uint64_t tx_seq;	/* extension state for the tx counter */
	uint64_t rev_seq;	/* extension state for reverse sequenceId */
	int64_t stop_at;	/* drain deadline once sending is over; 0 while sending */
};

/* Run a session against the sink named by opts->argv[0] until the
 * count is reached, the sink goes away or the user interrupts.
 * Returns 0, or 1 if the session could not be set up or was lost. */
int source_run(struct source *src, struct opts *opts,
	       const struct source_ops *ops, void *priv);

/* Where in the cycle a timestamp of this session falls, and which
 * cycle; TS_NONE without a window or a timestamp. */
static inline int64_t source_phase(const struct source *src, int64_t ts)
{
	return sched_phase(&src->sched, ts, src->mode);
}

static inline int64_t source_cycle(const struct source *src, int64_t ts)
{
	return sched_cycle(&src->sched, ts, src->mode);
}

static inline enum sched_tag source_tag(const struct source *src,
					const struct flow_sample *s)
{
	return sched_tag(&src->sched, source_phase(src, s->tx),
			 source_phase(src, s->rx));
}

#endif

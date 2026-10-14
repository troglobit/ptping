#ifndef PTPING_FLOW_H
#define PTPING_FLOW_H

#include <stdint.h>

/* One direction of probes, numbered from 0. Samples are created when
 * a probe is sent (forward flow) or when a timestamp for a sequence
 * number arrives (reverse flow), and resolved when both timestamps
 * are in or the loss timeout has passed. Resolved samples are
 * delivered in sequence order, so a complete sample waits behind an
 * incomplete one until the latter times out. */

struct flow_sample {
	uint64_t seq;
	int64_t tx;		/* TS_NONE if missing */
	int64_t rx;		/* TS_NONE if missing */
	int64_t correction;	/* raw correctionField */
};

typedef void (*flow_cb)(void *arg, const struct flow_sample *s);

struct flow_entry {
	struct flow_sample s;
	int64_t created;	/* CLOCK_MONOTONIC */
};

struct flow {
	struct flow_entry *ring;
	uint64_t mask;
	uint64_t head, tail;	/* unresolved: [head, tail) */

	int64_t timeout_ns;
	flow_cb cb;
	void *arg;

	uint64_t late;		/* timestamps for already resolved samples */
	uint64_t skipped;	/* never delivered: a jump beyond the window */
};

/* window: the number of unresolved samples to hold, rounded up to a
 * power of two. When a new sample would exceed it, the oldest is
 * resolved as is. Returns 0 or -1 if out of memory. */
int flow_init(struct flow *f, int64_t timeout_ns, uint64_t window,
	      flow_cb cb, void *arg);
void flow_fini(struct flow *f);

/* Create the sample for seq, and any missing ones before it. */
void flow_add(struct flow *f, uint64_t seq, int64_t now);

void flow_tx(struct flow *f, uint64_t seq, int64_t ts, int64_t now);
void flow_rx(struct flow *f, uint64_t seq, int64_t ts, int64_t correction,
	     int64_t now);

/* Resolve samples whose timeout has passed. Pass INT64_MAX as now
 * to flush everything. */
void flow_sweep(struct flow *f, int64_t now);

static inline uint64_t flow_outstanding(const struct flow *f)
{
	return f->tail - f->head;
}

#endif

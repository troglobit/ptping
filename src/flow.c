#include <stdlib.h>

#include "flow.h"
#include "tspec.h"

static struct flow_entry *entry(struct flow *f, uint64_t seq)
{
	return &f->ring[seq & f->mask];
}

static void resolve_head(struct flow *f)
{
	f->cb(f->arg, &entry(f, f->head++)->s);
}

static void resolve_complete(struct flow *f)
{
	struct flow_sample *s;

	while (f->head != f->tail) {
		s = &entry(f, f->head)->s;
		if (s->tx == TS_NONE || s->rx == TS_NONE)
			break;
		resolve_head(f);
	}
}

int flow_init(struct flow *f, int64_t timeout_ns, uint64_t window,
	      flow_cb cb, void *arg)
{
	uint64_t size = 1;

	while (size < window)
		size <<= 1;

	*f = (struct flow){
		.ring = malloc(size * sizeof(*f->ring)),
		.mask = size - 1,
		.timeout_ns = timeout_ns,
		.cb = cb,
		.arg = arg,
	};
	return f->ring ? 0 : -1;
}

void flow_fini(struct flow *f)
{
	free(f->ring);
	f->ring = NULL;
}

/* Returns the entry for seq, creating it and any missing ones before
 * it, or NULL if seq has already been resolved. */
static struct flow_entry *lookup(struct flow *f, uint64_t seq, int64_t now)
{
	struct flow_entry *e;

	if (seq < f->head) {
		f->late++;
		return NULL;
	}

	/* A jump past the whole window would create and evict every
	 * sample in between; account for them in bulk instead. */
	if (seq > f->tail + f->mask) {
		flow_sweep(f, INT64_MAX);
		f->skipped += seq - f->tail - f->mask;
		f->head = f->tail = seq - f->mask;
	}

	while (seq >= f->tail) {
		if (f->tail - f->head > f->mask)
			resolve_head(f);

		e = entry(f, f->tail);
		e->s = (struct flow_sample){
			.seq = f->tail,
			.tx = TS_NONE,
			.rx = TS_NONE,
		};
		e->created = now;
		f->tail++;
	}

	return entry(f, seq);
}

void flow_add(struct flow *f, uint64_t seq, int64_t now)
{
	lookup(f, seq, now);
}

/* Record one side's timestamp; the other side is left alone. */
static void stamp(struct flow *f, uint64_t seq, int64_t now,
		  int64_t tx, int64_t rx, int64_t correction)
{
	struct flow_entry *e = lookup(f, seq, now);

	if (!e)
		return;

	if (tx != TS_NONE)
		e->s.tx = tx;
	if (rx != TS_NONE) {
		e->s.rx = rx;
		e->s.correction = correction;
	}
	resolve_complete(f);
}

void flow_tx(struct flow *f, uint64_t seq, int64_t ts, int64_t now)
{
	stamp(f, seq, now, ts, TS_NONE, 0);
}

void flow_rx(struct flow *f, uint64_t seq, int64_t ts, int64_t correction,
	     int64_t now)
{
	stamp(f, seq, now, TS_NONE, ts, correction);
}

void flow_sweep(struct flow *f, int64_t now)
{
	while (f->head != f->tail &&
	       now - entry(f, f->head)->created >= f->timeout_ns)
		resolve_head(f);
	resolve_complete(f);
}

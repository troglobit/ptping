#ifndef PTPING_STATS_H
#define PTPING_STATS_H

#include <stdint.h>

/* Log-linear histogram of non-negative nanosecond values: one row per
 * power of two, each split into 128 linear buckets, so the relative
 * error of a percentile is below 1 %. Row 0 holds 0..127 exactly. */
#define HIST_SUB_BITS 7
#define HIST_SUB (1 << HIST_SUB_BITS)
#define HIST_ROWS (64 - HIST_SUB_BITS + 1)

struct hist {
	uint64_t bucket[HIST_ROWS][HIST_SUB];
};

struct stats {
	uint64_t sent;		/* samples created */
	uint64_t received;	/* samples with both timestamps */
	uint64_t lost;		/* rx timestamp never arrived */
	uint64_t no_tx;		/* tx timestamp never arrived */

	/* Over received samples only. */
	int64_t min, max;
	double mean, m2;	/* Welford */
	struct hist hist;
};

void stats_init(struct stats *st);

/* Account for a resolved sample. Pass TS_NONE for a missing side. */
void stats_add(struct stats *st, int64_t tx, int64_t rx);

double stats_mdev(const struct stats *st);

/* Smallest latency that at least fraction p (0 < p <= 1) of the
 * received samples are at or below. Negative latencies, which a clock
 * offset can produce, count as 0 here but not in min/max/mean. */
int64_t stats_percentile(const struct stats *st, double p);

#endif

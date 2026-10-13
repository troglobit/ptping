#include <math.h>
#include <string.h>

#include "stats.h"
#include "tspec.h"

static void hist_add(struct hist *h, uint64_t v)
{
	unsigned row = 0, sub, msb;

	if (v < HIST_SUB) {
		sub = v;
	} else {
		msb = 63 - __builtin_clzll(v);
		row = msb - HIST_SUB_BITS + 1;
		sub = (v >> (msb - HIST_SUB_BITS)) & (HIST_SUB - 1);
	}

	h->bucket[row][sub]++;
}

/* Largest value that lands in the bucket. */
static int64_t hist_bucket_max(unsigned row, unsigned sub)
{
	unsigned shift;

	if (!row)
		return sub;

	shift = row - 1;
	return (((int64_t)sub + HIST_SUB + 1) << shift) - 1;
}

static int64_t hist_percentile(const struct hist *h, uint64_t count, double p)
{
	uint64_t need = ceil(p * count), seen = 0;
	unsigned row, sub;

	for (row = 0; row < HIST_ROWS; row++) {
		for (sub = 0; sub < HIST_SUB; sub++) {
			seen += h->bucket[row][sub];
			if (seen >= need)
				return hist_bucket_max(row, sub);
		}
	}

	return 0;
}

void stats_init(struct stats *st)
{
	memset(st, 0, sizeof(*st));
	st->min = INT64_MAX;
	st->max = INT64_MIN;
}

void stats_add(struct stats *st, int64_t tx, int64_t rx)
{
	int64_t lat;
	double delta;

	st->sent++;
	if (tx == TS_NONE)
		st->no_tx++;
	if (rx == TS_NONE)
		st->lost++;
	if (tx == TS_NONE || rx == TS_NONE)
		return;

	lat = rx - tx;
	st->received++;
	if (lat < st->min)
		st->min = lat;
	if (lat > st->max)
		st->max = lat;

	delta = lat - st->mean;
	st->mean += delta / st->received;
	st->m2 += delta * (lat - st->mean);

	/* A clock offset can make the latency negative; the histogram
	 * is for non-negative values. */
	hist_add(&st->hist, lat < 0 ? 0 : lat);
}

double stats_mdev(const struct stats *st)
{
	return st->received ? sqrt(st->m2 / st->received) : 0;
}

int64_t stats_percentile(const struct stats *st, double p)
{
	return hist_percentile(&st->hist, st->received, p);
}

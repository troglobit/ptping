#include <math.h>

#include "stats.h"
#include "test.h"
#include "tspec.h"

static void test_counters(void)
{
	struct stats st;

	stats_init(&st);
	check(st.sent == 0 && stats_mdev(&st) == 0 && stats_percentile(&st, 0.5) == 0);

	stats_add(&st, 100, 150);
	stats_add(&st, TS_NONE, 150);
	stats_add(&st, 100, TS_NONE);
	stats_add(&st, TS_NONE, TS_NONE);

	check(st.sent == 4);
	check(st.received == 1);
	check(st.lost == 2);
	check(st.no_tx == 2);
	check(st.min == 50 && st.max == 50 && st.mean == 50);
	check(stats_mdev(&st) == 0);
}

static void test_moments(void)
{
	/* mean 5, population stddev 2 */
	static const int64_t v[] = { 2, 4, 4, 4, 5, 5, 7, 9 };
	struct stats st;
	size_t i;

	stats_init(&st);
	for (i = 0; i < sizeof(v) / sizeof(v[0]); i++)
		stats_add(&st, 0, v[i]);
	check(st.min == 2 && st.max == 9);
	check(fabs(st.mean - 5) < 1e-9);
	check(fabs(stats_mdev(&st) - 2) < 1e-9);

	/* Negative latencies are kept in min/mean, clamped in percentiles. */
	stats_init(&st);
	for (i = 0; i < 20; i++)
		stats_add(&st, 0, (int64_t)i - 10);
	check(st.min == -10 && st.max == 9);
	check(fabs(st.mean + 0.5) < 1e-9);
	check(stats_percentile(&st, 0.5) == 0);
	check(stats_percentile(&st, 1.0) == 9);
}

static void within_1pct(int64_t got, int64_t want)
{
	check(got >= want && got <= want + want / 100 + 1);
}

static void test_percentiles(void)
{
	struct stats st;
	int64_t i;

	/* Row 0 is exact. */
	stats_init(&st);
	for (i = 0; i < HIST_SUB; i++)
		stats_add(&st, 0, i);
	check(stats_percentile(&st, 0.5) == 63);
	check(stats_percentile(&st, 1.0) == 127);
	check(stats_percentile(&st, 1.0 / HIST_SUB) == 0);

	/* Microsecond range, uniform 1..100000 ns. */
	stats_init(&st);
	for (i = 1; i <= 100000; i++)
		stats_add(&st, 0, i);
	within_1pct(stats_percentile(&st, 0.5), 50000);
	within_1pct(stats_percentile(&st, 0.99), 99000);
	within_1pct(stats_percentile(&st, 0.999), 99900);
	within_1pct(stats_percentile(&st, 1.0), 100000);

	/* Seconds range, and a single huge outlier. */
	stats_init(&st);
	for (i = 0; i < 1000; i++)
		stats_add(&st, 0, 2 * NS_PER_S + i * NS_PER_MS);
	stats_add(&st, 0, 3600 * NS_PER_S);
	within_1pct(stats_percentile(&st, 0.5), 2 * NS_PER_S + 499 * NS_PER_MS);
	within_1pct(stats_percentile(&st, 1.0), 3600 * NS_PER_S);
	within_1pct(stats_percentile(&st, 0.999), 2 * NS_PER_S + 999 * NS_PER_MS);

	/* Largest representable value does not overflow the index. */
	stats_init(&st);
	stats_add(&st, 0, INT64_MAX);
	check(stats_percentile(&st, 1.0) >= INT64_MAX - INT64_MAX / 100);
}

int main(void)
{
	test_counters();
	test_moments();
	test_percentiles();
	return 0;
}

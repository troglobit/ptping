#include <errno.h>

#include "sched.h"
#include "test.h"
#include "tspec.h"

static void test_rate(void)
{
	struct sched s;
	int64_t t0, t1;

	check(!sched_init(&s, 1000, NULL, 0));
	check(s.clock == CLOCK_MONOTONIC && s.tai_offset == 0 && s.fd >= 0);
	sched_fini(&s);
	check(s.fd == -1);
	t0 = sched_next(&s);
	t1 = sched_next(&s);
	check(t1 == t0 + 1000);
	check(sched_next(&s) == t0 + 2000);
	check(sched_txtime(&s) == TS_NONE);
	check(sched_phase(&s, t0, TS_MODE_HW) == -1);
}

static void test_window(void)
{
	/* 1 ms cycle, window from 200 us to 500 us, 100 us period. */
	struct sched_window win = {
		.cycle_ns = 1000000,
		.base = 7,
		.offset_ns = 200000,
		.len_ns = 300000,
	};
	struct sched s;
	int64_t t, first;
	int i;

	check(!sched_init(&s, 100000, &win, 0));
	check(s.clock == CLOCK_REALTIME);

	/* The first due time is in a window, and the following ones
	 * step through it and jump to the next one. */
	first = sched_next(&s);
	check(sched_in_window(&s, sched_phase(&s, first, TS_MODE_HW)));
	for (i = 1; i < 10; i++) {
		t = sched_next(&s);
		check(sched_in_window(&s, sched_phase(&s, t, TS_MODE_HW)));
		check(t > first);
	}

	/* From a known phase: before, inside and after the window. */
	s.next = win.base + 5 * 1000000;	/* phase 0 */
	check(sched_phase(&s, sched_next(&s), TS_MODE_HW) == 200000);
	s.next = win.base + 5 * 1000000 + 250000;
	check(sched_phase(&s, sched_next(&s), TS_MODE_HW) == 250000);
	s.next = win.base + 5 * 1000000 + 500000;	/* just closed */
	t = sched_next(&s);
	check(t == win.base + 6 * 1000000 + 200000);
	check(sched_next(&s) == t + 100000);
	check(sched_next(&s) == t + 200000);
	/* 500000 is closed again: on to the window after. */
	check(sched_next(&s) == t + 1000000);

	/* A window shorter than the period: one probe per cycle. */
	win.len_ns = 10000;
	check(!sched_init(&s, 100000, &win, 0));
	s.next = win.base;
	t = sched_next(&s);
	check(sched_phase(&s, t, TS_MODE_HW) == 200000);
	check(sched_next(&s) == t + 1000000);

	/* A base time in the future keeps the phase consistent. */
	win.base = s.next + 10 * 1000000 + 123;
	check(!sched_init(&s, 100000, &win, 0));
	s.next = win.base - 3 * 1000000 + 123456;	/* phase 123456 */
	t = sched_next(&s);
	check(t == win.base - 3 * 1000000 + 200000);
	check(sched_phase(&s, t, TS_MODE_HW) == 200000);

	/* Software timestamps are shifted into the schedule's clock. */
	check(sched_phase(&s, t - s.tai_offset, TS_MODE_SW) == 200000);

	/* Inconsistent windows. */
	win.offset_ns = 900000;
	win.len_ns = 200000;
	check(sched_init(&s, 100000, &win, 0) == -1 && errno == EINVAL);
	win.len_ns = 0;
	check(sched_init(&s, 100000, &win, 0) == -1);
	check(sched_init(&s, 100000, NULL, -1) == -1);
}

static void test_txtime(void)
{
	struct sched s;
	int64_t t;

	check(!sched_init(&s, 1000000, NULL, 500000));
	check(s.clock == CLOCK_REALTIME);
	t = sched_next(&s);
	s.due = t;
	check(sched_txtime(&s) == t);
}

int main(void)
{
	test_rate();
	test_window();
	test_txtime();
	return 0;
}

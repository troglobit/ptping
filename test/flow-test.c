#include "flow.h"
#include "test.h"
#include "tspec.h"

static struct flow_sample out[64];
static unsigned nout;

static void cb(void *arg, const struct flow_sample *s)
{
	check(arg == &nout);
	check(nout < 64);
	out[nout++] = *s;
}

static void setup(struct flow *f, uint64_t window)
{
	check(!flow_init(f, 1000, window, cb, &nout));
	nout = 0;
}

static void test_forward(void)
{
	struct flow f;

	setup(&f, 16);

	/* Send 0, 1, 2; tx stamps arrive; rx for 1 before 0. */
	flow_add(&f, 0, 0);
	flow_add(&f, 1, 10);
	flow_add(&f, 2, 20);
	check(flow_outstanding(&f) == 3);
	flow_tx(&f, 0, 100, 1);
	flow_tx(&f, 1, 110, 11);
	flow_tx(&f, 2, 120, 21);
	check(nout == 0);

	flow_rx(&f, 1, 160, 7, 30);
	check(nout == 0);
	flow_rx(&f, 0, 150, 0, 31);
	check(nout == 2);
	check(out[0].seq == 0 && out[0].tx == 100 && out[0].rx == 150);
	check(out[1].seq == 1 && out[1].tx == 110 && out[1].rx == 160 && out[1].correction == 7);
	check(flow_outstanding(&f) == 1);

	/* 2 is lost: sweeping before the timeout keeps it, after resolves it. */
	flow_sweep(&f, 1019);
	check(nout == 2);
	flow_sweep(&f, 1020);
	check(nout == 3);
	check(out[2].seq == 2 && out[2].tx == 120 && out[2].rx == TS_NONE);
	check(flow_outstanding(&f) == 0);

	/* A late rx for 2 is dropped and counted. */
	flow_rx(&f, 2, 170, 0, 1021);
	check(nout == 3 && f.late == 1);

	/* Missing tx timestamp. */
	flow_add(&f, 3, 2000);
	flow_rx(&f, 3, 180, 0, 2001);
	check(nout == 3);
	flow_sweep(&f, 3000);
	check(nout == 4 && out[3].tx == TS_NONE && out[3].rx == 180);

	flow_fini(&f);
}

static void test_reverse(void)
{
	struct flow f;

	setup(&f, 16);

	/* The first timestamp seen is seq 2, so 0 and 1 are created too
	 * and time out as lost. */
	flow_rx(&f, 2, 500, 0, 0);
	check(f.head == 0 && f.tail == 3);
	flow_tx(&f, 2, 450, 1);
	check(nout == 0);
	flow_sweep(&f, 1000);
	check(nout == 3);
	check(out[0].seq == 0 && out[0].tx == TS_NONE && out[0].rx == TS_NONE);
	check(out[1].seq == 1 && out[1].tx == TS_NONE && out[1].rx == TS_NONE);
	check(out[2].seq == 2 && out[2].tx == 450 && out[2].rx == 500);

	flow_fini(&f);
}

static void test_window(void)
{
	struct flow f;
	unsigned i;

	setup(&f, 4);
	check(f.mask == 3);

	for (i = 0; i < 4; i++)
		flow_add(&f, i, i);
	check(nout == 0 && flow_outstanding(&f) == 4);

	/* A fifth sample evicts the oldest, unresolved. */
	flow_add(&f, 4, 4);
	check(nout == 1 && out[0].seq == 0 && out[0].tx == TS_NONE);
	check(flow_outstanding(&f) == 4);

	/* A jump within the window creates intermediate samples,
	 * evicting as needed. */
	flow_add(&f, 7, 7);
	check(nout == 4 && out[3].seq == 3);
	check(f.head == 4 && f.tail == 8);

	/* A jump beyond the window flushes, and reports the samples
	 * that could never have fit as skipped. */
	flow_add(&f, 20, 20);
	check(nout == 8 && out[7].seq == 7);
	check(f.skipped == 20 - 8 - 3);
	check(f.head == 17 && f.tail == 21);

	/* Flush. */
	flow_sweep(&f, INT64_MAX);
	check(nout == 12 && out[8].seq == 17 && out[11].seq == 20);
	check(flow_outstanding(&f) == 0);

	flow_fini(&f);
}

int main(void)
{
	test_forward();
	test_reverse();
	test_window();
	return 0;
}

#include <stdio.h>
#include <unistd.h>

#include "ptping.h"
#include "source.h"
#include "stats.h"
#include "tspec.h"

struct stat_priv {
	struct stats stats[2];
	struct stats tagged[2][SCHED_TAGS];	/* per tag, with a window */
	int tty;
	int64_t last;		/* time of the previous report, 0 before the first */
	uint64_t last_sent[2], last_received[2];
};

static double us(double ns)
{
	return ns / NS_PER_US;
}

static void latency_line(const char *label, const struct stats *st)
{
	printf("  %s: ", label);
	if (st->received)
		printf("%llu, %.3f/%.3f/%.3f/%.3f us min/avg/max/mdev, "
		       "%.3f/%.3f us p99/p99.9\n",
		       (unsigned long long)st->received,
		       us(st->min), us(st->mean), us(st->max), us(stats_mdev(st)),
		       us(stats_percentile(st, 0.99)),
		       us(stats_percentile(st, 0.999)));
	else
		printf("-\n");
}

/* Rates are over the interval, or over the whole run in the final
 * report. A fixed number of lines, so a terminal redraw can count on
 * it: three, plus one per tag with a window. */
static void report_flow(struct stat_priv *p, const struct source *src,
			enum ctrl_flow which, double seconds, int final)
{
	const struct stats *st = &p->stats[which];
	uint64_t sent = st->sent + flow_outstanding(&src->flow[which].flow);
	uint64_t received = st->sent - st->lost;
	uint64_t sent_base = final ? 0 : p->last_sent[which];
	uint64_t received_base = final ? 0 : p->last_received[which];
	double tx_rate = (sent - sent_base) / seconds;
	double rx_rate = (received - received_base) / seconds;
	enum sched_tag tag;

	p->last_sent[which] = sent;
	p->last_received[which] = received;

	printf("%s:\n", ctrl_flow_names[which]);
	printf("  packets: %llu/%.0fpps sent, %llu/%.0fpps received, %llu lost (%.1f%%)",
	       (unsigned long long)sent, tx_rate,
	       (unsigned long long)received, rx_rate,
	       (unsigned long long)st->lost,
	       st->sent ? 100.0 * st->lost / st->sent : 0.0);
	if (st->no_tx)
		printf(", %llu without tx timestamp", (unsigned long long)st->no_tx);
	printf("\n");

	latency_line("latency", st);

	if (src->sched.win.cycle_ns) {
		for (tag = TAG_CONFORMANT; tag < SCHED_TAGS; tag++)
			latency_line(sched_tag_names[tag], &p->tagged[which][tag]);
	}
}

static void sample(void *priv, const struct source *src, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct stat_priv *p = priv;

	stats_add(&p->stats[flow], s->tx, s->rx);
	stats_add(&p->tagged[flow][source_tag(src, s)], s->tx, s->rx);
}

static void report(void *priv, const struct source *src, int final)
{
	struct stat_priv *p = priv;
	int64_t now = ts_now(CLOCK_MONOTONIC);
	int64_t since = (final || !p->last) ? src->started : p->last;
	double seconds = (now - since) / (double)NS_PER_S;
	int flows = src->opts->bidir ? 2 : 1, which;
	int lines = 3 + (src->sched.win.cycle_ns ? SCHED_TAGS - 1 : 0);

	if (seconds <= 0)
		seconds = 1;

	/* On a terminal, move up over the previous report and erase it. */
	if (p->tty && p->last)
		printf("\033[%dA\033[J", lines * flows);
	p->last = now;

	for (which = 0; which < flows; which++)
		report_flow(p, src, which, seconds, final);

	if (!p->tty && !final)
		printf("\n");
	fflush(stdout);
}

int cmd_stat(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample, .report = report };
	static struct stat_priv priv;	/* half a megabyte of histograms */
	struct source src;
	unsigned i, j;

	priv.tty = isatty(STDOUT_FILENO);
	for (i = 0; i < 2; i++) {
		stats_init(&priv.stats[i]);
		for (j = 0; j < SCHED_TAGS; j++)
			stats_init(&priv.tagged[i][j]);
	}

	return source_run(&src, opts, &ops, &priv);
}

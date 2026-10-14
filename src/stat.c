#include <stdio.h>
#include <unistd.h>

#include "ptping.h"
#include "source.h"
#include "stats.h"
#include "tspec.h"

struct stat_priv {
	struct stats stats[2];
	uint64_t outside[2];	/* transmitted outside the window */
	int tty;
	int64_t last;		/* time of the previous report, 0 before the first */
	uint64_t last_sent[2], last_received[2];
};

static double us(double ns)
{
	return ns / NS_PER_US;
}

/* Rates are over the interval, or over the whole run in the final
 * report. Always three lines, so a terminal redraw can count on it. */
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
	if (src->sched.win.cycle_ns)
		printf(", %llu outside the window", (unsigned long long)p->outside[which]);
	printf("\n");

	if (st->received)
		printf("  latency: %.3f/%.3f/%.3f/%.3f us min/avg/max/mdev, "
		       "%.3f/%.3f us p99/p99.9\n",
		       us(st->min), us(st->mean), us(st->max), us(stats_mdev(st)),
		       us(stats_percentile(st, 0.99)),
		       us(stats_percentile(st, 0.999)));
	else
		printf("  latency: -\n");
}

static void sample(void *priv, const struct source *src, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct stat_priv *p = priv;
	int64_t phase = source_tx_phase(src, s->tx);

	stats_add(&p->stats[flow], s->tx, s->rx);
	if (phase != TS_NONE && !sched_in_window(&src->sched, phase))
		p->outside[flow]++;
}

static void report(void *priv, const struct source *src, int final)
{
	struct stat_priv *p = priv;
	int64_t now = ts_now(CLOCK_MONOTONIC);
	int64_t since = (final || !p->last) ? src->started : p->last;
	double seconds = (now - since) / (double)NS_PER_S;
	int flows = src->opts->bidir ? 2 : 1, which;

	if (seconds <= 0)
		seconds = 1;

	/* On a terminal, move up over the previous report and erase it. */
	if (p->tty && p->last)
		printf("\033[%dA\033[J", 3 * flows);
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
	struct source src;
	struct stat_priv priv = { .tty = isatty(STDOUT_FILENO) };

	stats_init(&priv.stats[CTRL_FLOW_FWD]);
	stats_init(&priv.stats[CTRL_FLOW_REV]);
	return source_run(&src, opts, &ops, &priv);
}

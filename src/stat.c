#include <stdarg.h>
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
	int lines;		/* printed by the report so far */
	int64_t last;		/* time of the previous report, 0 before the first */
	uint64_t last_sent[2], last_received[2];
};

static double us(double ns)
{
	return ns / NS_PER_US;
}

/* Every line of the report goes through here, so the redraw knows
 * how far up to go. */
static void line(struct stat_priv *p, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	p->lines++;
}

/* Rates are over the interval, or over the whole run in the final
 * report. */
static void packets_line(struct stat_priv *p, const struct source *src,
			 enum ctrl_flow which, double seconds, int final)
{
	const struct stats *st = &p->stats[which];
	uint64_t sent = st->sent + flow_outstanding(&src->flow[which].flow);
	uint64_t received = st->sent - st->lost;
	uint64_t sent_base = final ? 0 : p->last_sent[which];
	uint64_t received_base = final ? 0 : p->last_received[which];
	double tx_rate = (sent - sent_base) / seconds;
	double rx_rate = (received - received_base) / seconds;
	char no_tx[48] = "";

	p->last_sent[which] = sent;
	p->last_received[which] = received;

	if (st->no_tx)
		snprintf(no_tx, sizeof(no_tx), ", %llu without tx timestamp",
			 (unsigned long long)st->no_tx);

	line(p, "%s: %llu sent (%.0f pps), %llu received (%.0f pps), %llu lost (%.1f%%)%s",
	     ctrl_flow_names[which],
	     (unsigned long long)sent, tx_rate,
	     (unsigned long long)received, rx_rate,
	     (unsigned long long)st->lost,
	     st->sent ? 100.0 * st->lost / st->sent : 0.0, no_tx);
}

#define LABEL_WIDTH 14

static void table_row(struct stat_priv *p, const char *label, const struct stats *st)
{
	if (st->received)
		line(p, "%-*s %7llu %9.3f %9.3f %9.3f %9.3f %9.3f",
		     LABEL_WIDTH, label, (unsigned long long)st->received,
		     us(st->min), us(st->mean),
		     us(stats_percentile(st, 0.99)),
		     us(stats_percentile(st, 0.999)), us(st->max));
	else
		line(p, "%-*s %7llu %9s %9s %9s %9s %9s",
		     LABEL_WIDTH, label, 0ULL, "-", "-", "-", "-", "-");
}

/* One row for all samples, and one per tag with a window. With two
 * flows, a row naming the flow comes before its rows. */
static void table_flow(struct stat_priv *p, const struct source *src,
		       enum ctrl_flow which)
{
	enum sched_tag tag;

	if (src->opts->bidir)
		line(p, "%s", ctrl_flow_names[which]);

	table_row(p, "all", &p->stats[which]);
	if (src->sched.win.cycle_ns) {
		for (tag = TAG_INSIDE; tag < SCHED_TAGS; tag++)
			table_row(p, sched_tag_names[tag], &p->tagged[which][tag]);
	}
}

static void sample(void *priv, const struct source *src, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct stat_priv *p = priv;

	stats_add(&p->stats[flow], s->tx, s->rx);
	if (src->sched.win.cycle_ns)
		stats_add(&p->tagged[flow][sched_tag(&src->sched, source_phase(src, s->tx))],
			  s->tx, s->rx);
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
	if (p->tty && p->lines)
		printf("\033[%dA\033[J", p->lines);
	p->lines = 0;
	p->last = now;

	for (which = 0; which < flows; which++)
		packets_line(p, src, which, seconds, final);

	line(p, "");
	line(p, "%-*s %7s %9s %9s %9s %9s %9s", LABEL_WIDTH, "latency (us)",
	     "count", "min", "avg", "p99", "p99.9", "max");
	for (which = 0; which < flows; which++)
		table_flow(p, src, which);

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

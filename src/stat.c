#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "log.h"
#include "ptping.h"
#include "source.h"
#include "stats.h"
#include "tspec.h"

struct stream_stats {
	struct stats stats[2];
	struct stats tagged[2][SCHED_TAGS];	/* per tag, with a window */
	uint64_t last_sent[2], last_received[2];
};

struct stat_priv {
	struct stream_stats *ss;	/* per stream */
	int width;		/* of the stream names, 0 with one stream */
	int tty;
	int cols;		/* of the terminal, 0 if unknown */
	int rows;		/* printed by the report so far */
	int64_t last;		/* time of the previous report, 0 before the first */
};

static double us(double ns)
{
	return ns / NS_PER_US;
}

/* Every line of the report goes through here, so the redraw knows
 * how far up to go, counting the rows that a line wider than the
 * terminal wraps onto. */
static void line(struct stat_priv *p, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');

	p->rows += (p->cols && len > p->cols) ? (len - 1) / p->cols + 1 : 1;
}

/* Rates are over the interval, or over the whole run in the final
 * report. With several streams, each line starts with the stream's
 * name, padded so that the lines align. */
static void packets_line(struct stat_priv *p, const struct stream *st,
			 enum ctrl_flow which, double seconds, int final)
{
	struct stream_stats *ss = &p->ss[st->index];
	const struct stats *stats = &ss->stats[which];
	uint64_t sent = stats->sent + flow_outstanding(&st->flow[which].flow);
	uint64_t received = stats->sent - stats->lost;
	uint64_t sent_base = final ? 0 : ss->last_sent[which];
	uint64_t received_base = final ? 0 : ss->last_received[which];
	double tx_rate = (sent - sent_base) / seconds;
	double rx_rate = (received - received_base) / seconds;
	char no_tx[48] = "";

	ss->last_sent[which] = sent;
	ss->last_received[which] = received;

	if (stats->no_tx)
		snprintf(no_tx, sizeof(no_tx), ", %llu without tx timestamp",
			 (unsigned long long)stats->no_tx);

	line(p, "%-*s%s: %llu sent (%.0f pps), %llu received (%.0f pps), %llu lost (%.1f%%)%s",
	     p->width, p->width ? st->opts->name : "", ctrl_flow_names[which],
	     (unsigned long long)sent, tx_rate,
	     (unsigned long long)received, rx_rate,
	     (unsigned long long)stats->lost,
	     stats->sent ? 100.0 * stats->lost / stats->sent : 0.0, no_tx);
}

#define LABEL_WIDTH 14

static int flows(const struct stream *st)
{
	return st->opts->bidir ? 2 : 1;
}

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

/* One row for all samples, and one per tag with a window. With
 * several streams or two flows, a row naming them comes first. */
static void table_flow(struct stat_priv *p, const struct stream *st,
		       enum ctrl_flow which)
{
	struct stream_stats *ss = &p->ss[st->index];
	enum sched_tag tag;

	if (p->width && st->opts->bidir)
		line(p, "%s: %s", st->opts->name, ctrl_flow_names[which]);
	else if (p->width)
		line(p, "%s", st->opts->name);
	else if (st->opts->bidir)
		line(p, "%s", ctrl_flow_names[which]);

	table_row(p, "all", &ss->stats[which]);
	if (st->sched.win.cycle_ns) {
		for (tag = TAG_INSIDE; tag < SCHED_TAGS; tag++)
			table_row(p, sched_tag_names[tag], &ss->tagged[which][tag]);
	}
}

static void sample(void *priv, const struct stream *st, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct stat_priv *p = priv;
	struct stream_stats *ss = &p->ss[st->index];

	stats_add(&ss->stats[flow], s->tx, s->rx);
	if (st->sched.win.cycle_ns)
		stats_add(&ss->tagged[flow][sched_tag(&st->sched, stream_phase(st, s->tx))],
			  s->tx, s->rx);
}

static void report(void *priv, const struct source *src, int final)
{
	struct stat_priv *p = priv;
	int64_t now = ts_now(CLOCK_MONOTONIC);
	int64_t since = (final || !p->last) ? src->started : p->last;
	double seconds = (now - since) / (double)NS_PER_S;
	const struct stream *st;
	struct winsize ws;
	unsigned i;
	int which;

	if (seconds <= 0)
		seconds = 1;

	/* On a terminal, move up over the previous report and erase it. */
	if (p->tty && p->rows)
		printf("\033[%dA\033[J", p->rows);
	p->rows = 0;

	if (p->tty && !ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws))
		p->cols = ws.ws_col;
	p->last = now;

	for (i = 0; i < src->opts->nstreams; i++) {
		st = &src->streams[i];
		for (which = 0; which < flows(st); which++)
			packets_line(p, st, which, seconds, final);
	}

	line(p, "");
	line(p, "%-*s %7s %9s %9s %9s %9s %9s", LABEL_WIDTH, "latency (us)",
	     "count", "min", "avg", "p99", "p99.9", "max");
	for (i = 0; i < src->opts->nstreams; i++) {
		st = &src->streams[i];
		for (which = 0; which < flows(st); which++)
			table_flow(p, st, which);
	}

	if (!p->tty && !final)
		printf("\n");
	fflush(stdout);
}

int cmd_stat(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample, .report = report };
	struct stat_priv priv = { .tty = isatty(STDOUT_FILENO) };
	unsigned i, j, k;
	int len, ret;

	/* Half a megabyte of histograms per stream */
	priv.ss = calloc(opts->nstreams, sizeof(*priv.ss));
	if (!priv.ss)
		die_errno("stats");

	for (i = 0; i < opts->nstreams; i++) {
		for (j = 0; j < 2; j++) {
			stats_init(&priv.ss[i].stats[j]);
			for (k = 0; k < SCHED_TAGS; k++)
				stats_init(&priv.ss[i].tagged[j][k]);
		}

		len = strlen(opts->streams[i].name) + 2;
		if (opts->nstreams > 1 && len > priv.width)
			priv.width = len;
	}

	ret = source_run(opts, &ops, &priv);
	free(priv.ss);
	return ret;
}

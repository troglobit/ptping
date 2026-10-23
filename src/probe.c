#include <stdio.h>
#include <stdlib.h>

#include "log.h"
#include "ptping.h"
#include "source.h"
#include "tspec.h"

/* Above this, something in the path forwarded the probe in software. */
#define SLOW_PATH_NS NS_PER_MS

struct probe_priv {
	struct flow_sample (*samples)[2];	/* per stream and flow */
	int ret;
};

static void sample(void *priv, const struct stream *st, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct probe_priv *p = priv;

	p->samples[st->index][flow] = *s;
}

/* Returns 0 if the flow checks out. Every line starts with pfx. */
static int verdict(const char *pfx, const struct flow_sample *s,
		   enum ctrl_flow flow)
{
	const char *name = ctrl_flow_names[flow];
	int64_t lat;

	if (s->tx == TS_NONE && s->rx == TS_NONE) {
		printf("%s%s: lost\n", pfx, name);
		return 1;
	}
	if (s->tx == TS_NONE) {
		printf("%s%s: received, but no transmission timestamp\n", pfx, name);
		return 1;
	}
	if (s->rx == TS_NONE) {
		printf("%s%s: sent, but not received\n", pfx, name);
		return 1;
	}

	lat = s->rx - s->tx;
	printf("%s%s: latency %.3f us", pfx, name, lat / (double)NS_PER_US);
	if (s->correction)
		printf(", of which %.3f us residence time in transparent clocks",
		       s->correction / 65536.0 / NS_PER_US);
	printf("\n");

	if (lat > SLOW_PATH_NS) {
		printf("%s  warning: more than %lld ms; something in the path is "
		       "forwarding the probes in software\n", pfx,
		       (long long)(SLOW_PATH_NS / NS_PER_MS));
		return 1;
	}
	if (lat < 0) {
		printf("%s  warning: negative; the clocks are not synchronized\n", pfx);
		return 1;
	}

	return 0;
}

/* With several streams, every line starts with the stream's name. */
static void report(void *priv, const struct source *src, int final)
{
	struct probe_priv *p = priv;
	const struct stream *st;
	char pfx[256] = "";
	unsigned i;

	if (!final)
		return;

	for (i = 0; i < src->opts->nstreams; i++) {
		st = &src->streams[i];
		if (src->opts->nstreams > 1)
			snprintf(pfx, sizeof(pfx), "%s: ", st->opts->name);

		printf("%s%s timestamps on %s\n", pfx, ts_mode_str(st->mode), st->ni.name);
		p->ret |= verdict(pfx, &p->samples[i][CTRL_FLOW_FWD], CTRL_FLOW_FWD);
		if (st->opts->bidir)
			p->ret |= verdict(pfx, &p->samples[i][CTRL_FLOW_REV], CTRL_FLOW_REV);
	}
}

int cmd_probe(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample, .report = report };
	struct probe_priv priv = { 0 };
	unsigned i, j;
	int ret;

	priv.samples = calloc(opts->nstreams, sizeof(*priv.samples));
	if (!priv.samples)
		die_errno("samples");

	for (i = 0; i < opts->nstreams; i++) {
		opts->streams[i].count = 1;
		for (j = 0; j < 2; j++)
			priv.samples[i][j] = (struct flow_sample){ .tx = TS_NONE, .rx = TS_NONE };
	}

	ret = source_run(opts, &ops, &priv) || priv.ret;
	free(priv.samples);
	return ret;
}

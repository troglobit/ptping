#include <stdio.h>
#include <string.h>

#include "log.h"
#include "ptping.h"
#include "source.h"
#include "tspec.h"

static const char *flow_name[] = {
	[CTRL_FLOW_FWD] = "fwd",
	[CTRL_FLOW_REV] = "rev",
};

/* An empty field for a missing timestamp. */
static const char *field(int64_t ts, char *buf)
{
	if (ts == TS_NONE)
		return "";

	sprintf(buf, "%lld", (long long)ts);
	return buf;
}

static void sample(void *priv, const struct stream *st, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	char tx[24], rx[24], lat[24], txp[24], rxp[24], cyc[24];
	int64_t latency = TS_NONE, cycles = TS_NONE;
	int64_t tx_phase = stream_phase(st, s->tx), rx_phase = stream_phase(st, s->rx);
	int64_t tx_cycle = stream_cycle(st, s->tx), rx_cycle = stream_cycle(st, s->rx);

	if (s->tx != TS_NONE && s->rx != TS_NONE)
		latency = s->rx - s->tx;
	if (tx_cycle != TS_NONE && rx_cycle != TS_NONE)
		cycles = rx_cycle - tx_cycle;

	fprintf(priv, "%s,%s,%llu,%s,%s,%lld,%s,%s,%s,%s,%s\n",
		st->opts->name, flow_name[flow],
		(unsigned long long)s->seq, field(s->tx, tx), field(s->rx, rx),
		(long long)s->correction, field(latency, lat),
		field(tx_phase, txp), field(rx_phase, rxp), field(cycles, cyc),
		sched_tag_names[sched_tag(&st->sched, tx_phase)]);
}

static void report(void *priv, const struct source *src, int final)
{
	fflush(priv);
}

int cmd_record(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample, .report = report };
	FILE *fp = stdout;
	unsigned i;
	int ret;

	for (i = 0; i < opts->nstreams; i++) {
		if (strchr(opts->streams[i].name, ','))
			die("--name: \"%s\" has a comma, which would split its "
			    "CSV field", opts->streams[i].name);
	}

	if (opts->output) {
		fp = fopen(opts->output, "w");
		if (!fp)
			die_errno("%s", opts->output);
	}

	/* Lines go out once per report interval anyway. */
	setvbuf(fp, NULL, _IOFBF, 1 << 20);

	fprintf(fp, "stream,flow,seq,tx_ns,rx_ns,correction,latency_ns,"
		    "tx_phase_ns,rx_phase_ns,cycles,tag\n");
	ret = source_run(opts, &ops, fp);

	if (fp != stdout)
		fclose(fp);
	return ret;
}

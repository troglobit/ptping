#include <stdio.h>

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

static void sample(void *priv, const struct source *src, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	char tx[24], rx[24], lat[24], txp[24], rxp[24], cyc[24];
	int64_t latency = TS_NONE, cycles = TS_NONE;
	int64_t tx_phase = source_phase(src, s->tx), rx_phase = source_phase(src, s->rx);
	int64_t tx_cycle = source_cycle(src, s->tx), rx_cycle = source_cycle(src, s->rx);

	if (s->tx != TS_NONE && s->rx != TS_NONE)
		latency = s->rx - s->tx;
	if (tx_cycle != TS_NONE && rx_cycle != TS_NONE)
		cycles = rx_cycle - tx_cycle;

	fprintf(priv, "%s,%llu,%s,%s,%lld,%s,%s,%s,%s,%s\n", flow_name[flow],
		(unsigned long long)s->seq, field(s->tx, tx), field(s->rx, rx),
		(long long)s->correction, field(latency, lat),
		field(tx_phase, txp), field(rx_phase, rxp), field(cycles, cyc),
		sched_tag_names[sched_tag(&src->sched, tx_phase)]);
}

static void report(void *priv, const struct source *src, int final)
{
	fflush(priv);
}

int cmd_record(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample, .report = report };
	FILE *fp = stdout;
	struct source src;
	int ret;

	if (opts->argc == 2) {
		fp = fopen(opts->argv[1], "w");
		if (!fp)
			die_errno("%s", opts->argv[1]);
	}

	/* Lines go out once per report interval anyway. */
	setvbuf(fp, NULL, _IOFBF, 1 << 20);

	fprintf(fp, "flow,seq,tx_ns,rx_ns,correction,latency_ns,"
		    "tx_phase_ns,rx_phase_ns,cycles,tag\n");
	ret = source_run(&src, opts, &ops, fp);

	if (fp != stdout)
		fclose(fp);
	return ret;
}

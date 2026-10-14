#include <stdio.h>

#include "ptping.h"
#include "source.h"
#include "tspec.h"

/* Above this, something in the path forwarded the probe in software. */
#define SLOW_PATH_NS NS_PER_MS

static void sample(void *priv, const struct source *src, enum ctrl_flow flow,
		   const struct flow_sample *s)
{
	struct flow_sample *samples = priv;

	samples[flow] = *s;
}

/* Returns 0 if the flow checks out. */
static int verdict(const struct flow_sample *s, enum ctrl_flow flow)
{
	const char *name = ctrl_flow_names[flow];
	int64_t lat;

	if (s->tx == TS_NONE && s->rx == TS_NONE) {
		printf("%s: lost\n", name);
		return 1;
	}
	if (s->tx == TS_NONE) {
		printf("%s: received, but no transmission timestamp\n", name);
		return 1;
	}
	if (s->rx == TS_NONE) {
		printf("%s: sent, but not received\n", name);
		return 1;
	}

	lat = s->rx - s->tx;
	printf("%s: latency %.3f us", name, lat / (double)NS_PER_US);
	if (s->correction)
		printf(", of which %.3f us residence time in transparent clocks",
		       s->correction / 65536.0 / NS_PER_US);
	printf("\n");

	if (lat > SLOW_PATH_NS) {
		printf("  warning: more than %lld ms; something in the path is "
		       "forwarding the probes in software\n",
		       (long long)(SLOW_PATH_NS / NS_PER_MS));
		return 1;
	}
	if (lat < 0) {
		printf("  warning: negative; the clocks are not synchronized\n");
		return 1;
	}

	return 0;
}

int cmd_probe(struct opts *opts)
{
	static const struct source_ops ops = { .sample = sample };
	struct flow_sample samples[2] = {
		{ .tx = TS_NONE, .rx = TS_NONE },
		{ .tx = TS_NONE, .rx = TS_NONE },
	};
	struct source src;
	int ret;

	opts->count = 1;
	if (source_run(&src, opts, &ops, samples))
		return 1;

	printf("%s timestamps on %s\n", ts_mode_str(src.mode), src.ni.name);
	ret = verdict(&samples[CTRL_FLOW_FWD], CTRL_FLOW_FWD);
	if (opts->bidir)
		ret |= verdict(&samples[CTRL_FLOW_REV], CTRL_FLOW_REV);

	return ret;
}

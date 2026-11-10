#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ptp.h"
#include "ptping.h"
#include "tspec.h"

#include "config.h"

static const char *usage_text =
"usage: ptping [<common-options>] <command> [<command-options-and-arguments>]\n"
"\n"
"  ptping sink   [<BIND-ADDRESS>]\n"
"  ptping probe  <STREAM>...\n"
"  ptping stat   <STREAM>...\n"
"  ptping record [-o <FILE>] <STREAM>...\n"
"  ptping <STREAM>...\n"
"\n"
"A <STREAM> is [<source-options>] <SINK-ADDRESS>. Probes go to each\n"
"SINK-ADDRESS with all source options given before it, except --name,\n"
"which only names the next one.\n"
"\n"
"Common options:\n"
"  -p, --port N                  Probe and control port (default: 319)\n"
"  -i, --interval TIME           Time between stat reports (default: 1s)\n"
"  -o, --output FILE             Where record writes (default: stdout)\n"
"  -v, --verbose                 More output, repeat for even more\n"
"  -h, --help                    This text\n"
"  -V, --version                 Print version and exit\n"
"\n"
"Source options:\n"
"  -n, --name NAME               Name of the stream (default: SINK-ADDRESS)\n"
"  -D, --domain N                PTP domain (default: 254)\n"
"  -T, --timestamping hw|sw|any  Timestamping requirement (default: hw)\n"
"  -b, --bidir                   Also measure sink to source\n"
"      --no-bidir                Do not, again\n"
"  -c, --count N                 Stop after N probes\n"
"  -d, --dscp VAL|csN            DSCP of probes (default: cs5)\n"
"  -r, --rate RATE|TIME          Probes per second, or the time between\n"
"                                them: 100, 4.7hz, 10ms (default: 100hz)\n"
"  -s, --size BYTES              Frame size on the wire (default: minimum)\n"
"  -W, --timeout TIME            Time to wait for a probe (default: 1s)\n"
"\n"
"Schedule options (CLOCK_TAI):\n"
"      --cycle TIME              Cycle time of the schedule\n"
"      --window START-END        Send only within this part of the cycle;\n"
"      --window START+LENGTH     either form\n"
"      --base-time TIME          Cycles start here (default: 0)\n"
"      --observe                 Send throughout the cycle anyway, and tag\n"
"                                each probe by where it left\n"
"      --no-observe              Send only within the window again\n"
"      --txtime TIME             Set launch times for the etf qdisc, handing\n"
"                                probes to the kernel this much early\n"
"\n"
"A TIME takes a unit, ns, us, ms or s, and decimals: 1.5ms. Without a\n"
"unit, the schedule options are in nanoseconds and the rest in seconds.\n";

static void usage(int status)
{
	fputs(usage_text, status ? stderr : stdout);
	exit(status);
}

static unsigned long parse_ulong(const char *opt, const char *arg,
				 unsigned long min, unsigned long max)
{
	unsigned long val;
	char *end;

	errno = 0;
	val = strtoul(arg, &end, 0);
	if (errno || *end || val < min || val > max)
		die("%s: expected a number in [%lu, %lu], got \"%s\"",
		    opt, min, max, arg);

	return val;
}

/* A time of at least min_ns, with an optional unit, ns, us, ms or
 * s, and decimals: "1.5ms". Without a unit it is in unit_ns. Returns
 * nanoseconds. Integers are parsed as such, so an absolute nanosecond
 * timestamp stays exact. */
static int64_t parse_time(const char *opt, const char *arg, int64_t unit_ns,
			  int64_t min_ns)
{
	static const struct { const char *name; int64_t ns; } units[] = {
		{ "ns", 1 }, { "us", NS_PER_US }, { "ms", NS_PER_MS }, { "s", NS_PER_S },
	};
	int64_t whole, frac = 0, scale = 1, val;
	const char *p = arg;
	char *num_end;
	unsigned i, digits;

	errno = 0;
	whole = strtoll(p, &num_end, 10);
	if (errno || whole < 0)
		die("%s: expected a time, got \"%s\"", opt, arg);
	digits = num_end - p;
	p = num_end;

	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9'; p++, digits++) {
			if (scale < NS_PER_S) {
				frac = frac * 10 + (*p - '0');
				scale *= 10;
			}
		}
	}
	if (!digits)
		die("%s: expected a time, got \"%s\"", opt, arg);

	for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
		size_t n = strlen(units[i].name);

		if (!strncmp(p, units[i].name, n)) {
			unit_ns = units[i].ns;
			p += n;
			break;
		}
	}

	if (*p)
		die("%s: expected a time, got \"%s\"", opt, arg);
	if (whole > INT64_MAX / unit_ns)
		die("%s: \"%s\" is too large", opt, arg);

	val = whole * unit_ns + frac * unit_ns / scale;
	if (val < min_ns)
		die("%s: must be at least %lld ns", opt, (long long)min_ns);

	return val;
}

/* The time between probes, from a rate, "100" or "4.7hz", or from
 * the gap itself, "10ms" or "1.337ms". At most one per microsecond. */
static int64_t parse_period(const char *arg)
{
	int64_t period;
	double hz;
	char *end;

	errno = 0;
	hz = strtod(arg, &end);
	if (*end && strcmp(end, "hz"))
		period = parse_time("--rate", arg, 1, 0);
	else if (errno || end == arg || hz <= 0)
		die("--rate: expected a rate or a time, got \"%s\"", arg);
	else
		period = llround(NS_PER_S / hz);

	if (period < NS_PER_US)
		die("--rate: at most one probe per microsecond");

	return period;
}

/* "START-END" or "START+LENGTH"; arg is split in place. */
static void parse_window(char *arg, struct sched_window *win)
{
	char *sep = strpbrk(arg, "-+"), op;
	int64_t val;

	if (!sep)
		die("--window: expected START-END or START+LENGTH, got \"%s\"", arg);

	op = *sep;
	*sep = '\0';
	win->offset_ns = parse_time("--window", arg, 1, 0);
	val = parse_time("--window", sep + 1, 1, 0);

	win->len_ns = op == '+' ? val : val - win->offset_ns;
	if (win->len_ns <= 0)
		die("--window: the window must not be empty");
}

static uint8_t parse_dscp(const char *arg)
{
	if (!strncasecmp(arg, "cs", 2) && arg[2] >= '0' && arg[2] <= '7' && !arg[3])
		return (arg[2] - '0') << 3;

	return parse_ulong("--dscp", arg, 0, 63);
}

enum {
	OPT_CYCLE = 256,
	OPT_WINDOW,
	OPT_BASE_TIME,
	OPT_OBSERVE,
	OPT_NO_OBSERVE,
	OPT_NO_BIDIR,
	OPT_TXTIME,
};

struct cmd {
	const char *name;
	int (*fn)(struct opts *);
	bool streams;		/* takes SINK-ADDRESSes, else a BIND-ADDRESS */
};

static const struct cmd cmds[] = {
	{ "sink",   cmd_sink,   false },
	{ "probe",  cmd_probe,  true },
	{ "stat",   cmd_stat,   true },
	{ "record", cmd_record, true },
	{ NULL, NULL, false }
};

static const struct cmd *cmd_find(const char *name)
{
	const struct cmd *cmd;

	for (cmd = cmds; cmd->name; cmd++) {
		if (!strcmp(cmd->name, name))
			return cmd;
	}

	return NULL;
}

static int name_taken(const struct opts *opts, const char *name)
{
	unsigned i;

	for (i = 0; i < opts->nstreams; i++) {
		if (!strcmp(opts->streams[i].name, name))
			return 1;
	}

	return 0;
}

/* The sink's address, or if a stream already has that name, the
 * address with the first free "#2", "#3"... */
static const char *stream_name(const struct opts *opts, const char *sink)
{
	unsigned n;
	char *name;

	if (!name_taken(opts, sink))
		return sink;

	for (n = 2;; n++) {
		if (asprintf(&name, "%s#%u", sink, n) < 0)
			die_errno("asprintf");
		if (!name_taken(opts, name))
			return name;
		free(name);
	}
}

/* A stream to sink, with the options given so far. */
static void stream_add(struct opts *opts, struct stream_opts *cur, const char *sink)
{
	struct stream_opts *st;

	if (cur->win.cycle_ns && cur->win.offset_ns + cur->win.len_ns > cur->win.cycle_ns)
		die("--window must fit in the cycle");
	if (!cur->win.cycle_ns && (cur->win.len_ns || cur->win.base || cur->win.observe))
		die("--window, --base-time and --observe need --cycle");
	if (cur->win.cycle_ns && !cur->win.len_ns)
		die("--cycle needs --window");
	if (opts->nstreams == PTP_SELF_PORTS)
		die("at most %d streams", PTP_SELF_PORTS);

	opts->streams = realloc(opts->streams, (opts->nstreams + 1) * sizeof(*st));
	if (!opts->streams)
		die_errno("realloc");

	if (cur->name && name_taken(opts, cur->name))
		die("two streams are named \"%s\"; tell them apart with --name",
		    cur->name);

	st = &opts->streams[opts->nstreams];
	*st = *cur;
	st->sink = sink;
	if (!st->name)
		st->name = stream_name(opts, sink);
	cur->name = NULL;
	opts->nstreams++;
}

/* An option for the next SINK-ADDRESS. Returns -1 if c is not one. */
static int stream_opt(struct stream_opts *cur, int c, char *arg)
{
	switch (c) {
	case 'n':
		cur->name = arg;
		break;
	case 'D':
		cur->domain = parse_ulong("--domain", arg, 0, 255);
		break;
	case 'T':
		c = ts_mode_parse(arg);
		if (c < 0)
			die("--timestamping: expected hw, sw or any, got \"%s\"", arg);
		cur->ts_mode = c;
		break;
	case 'b':
		cur->bidir = true;
		break;
	case OPT_NO_BIDIR:
		cur->bidir = false;
		break;
	case 'c':
		cur->count = parse_ulong("--count", arg, 1, UINT32_MAX);
		break;
	case 'd':
		cur->dscp = parse_dscp(arg);
		break;
	case 'r':
		cur->period_ns = parse_period(arg);
		break;
	case 's':
		cur->size = parse_ulong("--size", arg, 1, 9000);
		break;
	case 'W':
		cur->timeout_ns = parse_time("--timeout", arg, NS_PER_S, 1);
		break;
	case OPT_CYCLE:
		cur->win.cycle_ns = parse_time("--cycle", arg, 1, 1);
		break;
	case OPT_WINDOW:
		parse_window(arg, &cur->win);
		break;
	case OPT_BASE_TIME:
		cur->win.base = parse_time("--base-time", arg, 1, 0);
		break;
	case OPT_OBSERVE:
		cur->win.observe = 1;
		break;
	case OPT_NO_OBSERVE:
		cur->win.observe = 0;
		break;
	case OPT_TXTIME:
		cur->txtime_lead_ns = parse_time("--txtime", arg, 1, 1);
		break;
	default:
		return -1;
	}

	return 0;
}

/* A command, or for the sink its bind address, or else a stream. */
static void positional(struct opts *opts, struct stream_opts *cur,
		       const struct cmd **cmd, const char *arg)
{
	if (!*cmd) {
		*cmd = cmd_find(arg);
		if (*cmd)
			return;

		/* "ptping [<source-options>] <SINK-ADDRESS>" */
		*cmd = cmd_find("stat");
	}

	if ((*cmd)->streams) {
		stream_add(opts, cur, arg);
		return;
	}

	if (opts->bind)
		usage(1);
	opts->bind = arg;
}

/* Options are taken in order, since each SINK-ADDRESS gets the source
 * options that come before it. Returns the command. */
static const struct cmd *opts_parse(struct opts *opts, int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "port",         required_argument, NULL, 'p' },
		{ "interval",     required_argument, NULL, 'i' },
		{ "output",       required_argument, NULL, 'o' },
		{ "verbose",      no_argument,       NULL, 'v' },
		{ "help",         no_argument,       NULL, 'h' },
		{ "version",      no_argument,       NULL, 'V' },

		{ "name",         required_argument, NULL, 'n' },
		{ "domain",       required_argument, NULL, 'D' },
		{ "timestamping", required_argument, NULL, 'T' },
		{ "bidir",        no_argument,       NULL, 'b' },
		{ "no-bidir",     no_argument,       NULL, OPT_NO_BIDIR },
		{ "count",        required_argument, NULL, 'c' },
		{ "dscp",         required_argument, NULL, 'd' },
		{ "rate",         required_argument, NULL, 'r' },
		{ "size",         required_argument, NULL, 's' },
		{ "timeout",      required_argument, NULL, 'W' },
		{ "cycle",        required_argument, NULL, OPT_CYCLE },
		{ "window",       required_argument, NULL, OPT_WINDOW },
		{ "base-time",    required_argument, NULL, OPT_BASE_TIME },
		{ "observe",      no_argument,       NULL, OPT_OBSERVE },
		{ "no-observe",   no_argument,       NULL, OPT_NO_OBSERVE },
		{ "txtime",       required_argument, NULL, OPT_TXTIME },
		{ NULL, 0, NULL, 0 }
	};
	struct stream_opts cur = {
		.ts_mode = TS_MODE_HW,
		.domain = 254,
		.dscp = 40, /* CS5 */
		.period_ns = NS_PER_S / 100,
		.timeout_ns = NS_PER_S,
	};
	const struct cmd *cmd = NULL;
	int c, unused = 0;

	while ((c = getopt_long(argc, argv, "-p:i:o:vhVn:D:T:bc:d:r:s:W:",
				longopts, NULL)) != -1) {
		switch (c) {
		case 1:
			positional(opts, &cur, &cmd, optarg);
			unused = 0;
			break;

		case 'p':
			opts->port = parse_ulong("--port", optarg, 1, 65535);
			break;
		case 'i':
			opts->report_ns = parse_time("--interval", optarg, NS_PER_S, 1);
			break;
		case 'o':
			opts->output = optarg;
			break;
		case 'v':
			log_level++;
			break;
		case 'h':
			usage(0);
			break;
		case 'V':
			puts(PACKAGE_STRING);
			exit(0);

		default:
			if (stream_opt(&cur, c, optarg))
				usage(1);
			unused = 1;
		}
	}

	/* After "--" */
	for (; optind < argc; optind++)
		positional(opts, &cur, &cmd, argv[optind]);

	if (!cmd || (cmd->streams && !opts->nstreams))
		usage(1);
	if (unused && cmd->streams)
		die("source options apply to the SINK-ADDRESS after them, and "
		    "there is none after the last ones");

	return cmd;
}

int main(int argc, char **argv)
{
	struct opts opts = {
		.port = PTPING_PORT,
		.report_ns = NS_PER_S,
	};
	const struct cmd *cmd;

	cmd = opts_parse(&opts, argc, argv);
	return cmd->fn(&opts);
}

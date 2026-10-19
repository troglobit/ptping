#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "ptping.h"
#include "tspec.h"

#include "config.h"

static const char *usage_text =
"usage: ptping [<common-options>] <command> [<command-options-and-arguments>]\n"
"\n"
"  ptping sink   [<BIND-ADDRESS>]\n"
"  ptping probe  [<source-options>] <SINK-ADDRESS>\n"
"  ptping stat   [<source-options>] <SINK-ADDRESS>\n"
"  ptping record [<source-options>] <SINK-ADDRESS> [<OUTPUT-FILE>]\n"
"  ptping [<source-options>] <SINK-ADDRESS>\n"
"\n"
"Common options:\n"
"  -D, --domain N                PTP domain (default: 254)\n"
"  -p, --port N                  Probe and control port (default: 319)\n"
"  -v, --verbose                 More output, repeat for even more\n"
"  -h, --help                    This text\n"
"  -V, --version                 Print version and exit\n"
"\n"
"Source options:\n"
"  -T, --timestamping hw|sw|any  Timestamping requirement (default: hw)\n"
"  -b, --bidir                   Also measure sink to source\n"
"  -c, --count N                 Stop after N probes\n"
"  -d, --dscp VAL|csN            DSCP of probes (default: cs5)\n"
"  -i, --interval TIME           Time between reports (default: 1s)\n"
"  -r, --rate PPS                Probes per second (default: 100)\n"
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
	OPT_TXTIME,
};

static void opts_parse(struct opts *opts, int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "timestamping", required_argument, NULL, 'T' },
		{ "domain",       required_argument, NULL, 'D' },
		{ "port",         required_argument, NULL, 'p' },
		{ "verbose",      no_argument,       NULL, 'v' },
		{ "help",         no_argument,       NULL, 'h' },
		{ "version",      no_argument,       NULL, 'V' },

		{ "bidir",        no_argument,       NULL, 'b' },
		{ "count",        required_argument, NULL, 'c' },
		{ "dscp",         required_argument, NULL, 'd' },
		{ "interval",     required_argument, NULL, 'i' },
		{ "rate",         required_argument, NULL, 'r' },
		{ "size",         required_argument, NULL, 's' },
		{ "timeout",      required_argument, NULL, 'W' },
		{ "cycle",        required_argument, NULL, OPT_CYCLE },
		{ "window",       required_argument, NULL, OPT_WINDOW },
		{ "base-time",    required_argument, NULL, OPT_BASE_TIME },
		{ "observe",      no_argument,       NULL, OPT_OBSERVE },
		{ "txtime",       required_argument, NULL, OPT_TXTIME },
		{ NULL }
	};
	int c;

	while ((c = getopt_long(argc, argv, "T:D:p:vhVbc:d:i:r:s:W:",
				longopts, NULL)) != -1) {
		switch (c) {
		case 'T':
			c = ts_mode_parse(optarg);
			if (c < 0)
				die("--timestamping: expected hw, sw or any, got \"%s\"", optarg);
			opts->ts_mode = c;
			break;
		case 'D':
			opts->domain = parse_ulong("--domain", optarg, 0, 255);
			break;
		case 'p':
			opts->port = parse_ulong("--port", optarg, 1, 65535);
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

		case 'b':
			opts->bidir = true;
			break;
		case 'c':
			opts->count = parse_ulong("--count", optarg, 1, UINT32_MAX);
			break;
		case 'd':
			opts->dscp = parse_dscp(optarg);
			break;
		case 'i':
			opts->report_ns = parse_time("--interval", optarg, NS_PER_S, 1);
			break;
		case 'r':
			opts->period_ns = NS_PER_S /
				parse_ulong("--rate", optarg, 1, 1000000);
			break;
		case 's':
			opts->size = parse_ulong("--size", optarg, 1, 9000);
			break;
		case 'W':
			opts->timeout_ns = parse_time("--timeout", optarg, NS_PER_S, 1);
			break;
		case OPT_CYCLE:
			opts->win.cycle_ns = parse_time("--cycle", optarg, 1, 1);
			break;
		case OPT_WINDOW:
			parse_window(optarg, &opts->win);
			break;
		case OPT_BASE_TIME:
			opts->win.base = parse_time("--base-time", optarg, 1, 0);
			break;
		case OPT_OBSERVE:
			opts->win.observe = 1;
			break;
		case OPT_TXTIME:
			opts->txtime_lead_ns = parse_time("--txtime", optarg, 1, 1);
			break;

		default:
			usage(1);
		}
	}

	if (opts->win.cycle_ns && opts->win.offset_ns + opts->win.len_ns > opts->win.cycle_ns)
		die("--window must fit in the cycle");
	if (!opts->win.cycle_ns && (opts->win.len_ns || opts->win.base || opts->win.observe))
		die("--window, --base-time and --observe need --cycle");
	if (opts->win.cycle_ns && !opts->win.len_ns)
		die("--cycle needs --window");

	opts->argc = argc - optind;
	opts->argv = argv + optind;
}

struct cmd {
	const char *name;
	int (*fn)(struct opts *);
	int min_args, max_args;
};

static const struct cmd cmds[] = {
	{ "sink",   cmd_sink,   0, 1 },
	{ "probe",  cmd_probe,  1, 1 },
	{ "stat",   cmd_stat,   1, 1 },
	{ "record", cmd_record, 1, 2 },
	{ NULL }
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

int main(int argc, char **argv)
{
	struct opts opts = {
		.ts_mode = TS_MODE_HW,
		.domain = 254,
		.port = PTPING_PORT,
		.dscp = 40, /* CS5 */
		.period_ns = NS_PER_S / 100,
		.report_ns = NS_PER_S,
		.timeout_ns = NS_PER_S,
	};
	const struct cmd *cmd;

	opts_parse(&opts, argc, argv);
	if (!opts.argc)
		usage(1);

	cmd = cmd_find(opts.argv[0]);
	if (cmd) {
		opts.argc--;
		opts.argv++;
	} else {
		/* "ptping [<source-options>] <SINK-ADDRESS>" */
		cmd = cmd_find("stat");
	}

	if (opts.argc < cmd->min_args || opts.argc > cmd->max_args)
		usage(1);

	return cmd->fn(&opts);
}

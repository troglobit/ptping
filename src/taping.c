#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "taping.h"
#include "tspec.h"

#include "config.h"

static const char *usage_text =
"usage: taping [<common-options>] <command> [<command-options-and-arguments>]\n"
"\n"
"  taping sink   [<BIND-ADDRESS>]\n"
"  taping probe  [<source-options>] <SINK-ADDRESS>\n"
"  taping stat   [<source-options>] <SINK-ADDRESS>\n"
"  taping record [<source-options>] <SINK-ADDRESS> [<OUTPUT-FILE>]\n"
"  taping [<source-options>] <SINK-ADDRESS>\n"
"\n"
"Common options:\n"
"  -T, --timestamping hw|sw|any  Timestamping requirement (default: hw)\n"
"  -D, --domain N                PTP domain (default: 254)\n"
"  -p, --port N                  Probe and control port (default: 319)\n"
"  -v, --verbose                 More output, repeat for even more\n"
"  -h, --help                    This text\n"
"  -V, --version                 Print version and exit\n"
"\n"
"Source options:\n"
"  -b, --bidir                   Also measure sink to source\n"
"  -c, --count N                 Stop after N probes\n"
"  -d, --dscp VAL|csN            DSCP of probes (default: cs5)\n"
"  -i, --interval SECONDS        Time between reports (default: 1)\n"
"  -r, --rate PPS                Probes per second (default: 100)\n"
"  -s, --size BYTES              Frame size on the wire (default: minimum)\n"
"  -W, --timeout SECONDS         Time to wait for a probe (default: 1)\n";

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

static int64_t parse_seconds(const char *opt, const char *arg)
{
	double val;
	char *end;

	errno = 0;
	val = strtod(arg, &end);
	if (errno || *end || val <= 0 || val > 86400)
		die("%s: expected a number of seconds, got \"%s\"", opt, arg);

	return (int64_t)(val * NS_PER_S);
}

static uint8_t parse_dscp(const char *arg)
{
	if (!strncasecmp(arg, "cs", 2) && arg[2] >= '0' && arg[2] <= '7' && !arg[3])
		return (arg[2] - '0') << 3;

	return parse_ulong("--dscp", arg, 0, 63);
}

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
			opts->report_ns = parse_seconds("--interval", optarg);
			break;
		case 'r':
			opts->period_ns = NS_PER_S /
				parse_ulong("--rate", optarg, 1, 1000000);
			break;
		case 's':
			opts->size = parse_ulong("--size", optarg, 1, 9000);
			break;
		case 'W':
			opts->timeout_ns = parse_seconds("--timeout", optarg);
			break;

		default:
			usage(1);
		}
	}

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
	{ "stat",   cmd_stat,   1, 1 },
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
		.port = TAPING_PORT,
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
		/* "taping [<source-options>] <SINK-ADDRESS>" */
		cmd = cmd_find("stat");
	}

	if (opts.argc < cmd->min_args || opts.argc > cmd->max_args)
		usage(1);

	return cmd->fn(&opts);
}

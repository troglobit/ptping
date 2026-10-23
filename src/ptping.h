#ifndef PTPING_H
#define PTPING_H

#include <stdbool.h>
#include <stdint.h>

#include "sched.h"
#include "ts.h"

/* The PTP event port, used for probes and the control connection. */
#define PTPING_PORT 319

/* What one stream of probes is sent with. Each SINK-ADDRESS on the
 * command line is a stream, with the options given before it. */
struct stream_opts {
	const char *name;
	const char *sink;
	enum ts_mode ts_mode;
	uint8_t domain;
	bool bidir;
	uint32_t count;
	uint8_t dscp;
	int64_t period_ns;	/* between probes */
	uint16_t size;		/* frame size, 0: minimum */
	int64_t timeout_ns;	/* loss timeout */
	struct sched_window win;
	int64_t txtime_lead_ns;
};

/* Everything the command line can say, for all commands. Each command
 * reads the fields that apply to it. */
struct opts {
	uint16_t port;
	int64_t report_ns;	/* between stat reports */
	const char *output;	/* record */
	const char *bind;	/* sink */

	struct stream_opts *streams;
	unsigned nstreams;
};

int cmd_sink(struct opts *opts);
int cmd_probe(struct opts *opts);
int cmd_stat(struct opts *opts);
int cmd_record(struct opts *opts);

#endif

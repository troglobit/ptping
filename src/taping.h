#ifndef TAPING_H
#define TAPING_H

#include <stdbool.h>
#include <stdint.h>

#include "ts.h"

/* The PTP event port, used for probes and the control connection. */
#define TAPING_PORT 319

/* Everything the command line can say, for all commands. Each command
 * reads the fields that apply to it. */
struct opts {
	enum ts_mode ts_mode;
	uint8_t domain;
	uint16_t port;

	bool bidir;
	uint32_t count;
	uint8_t dscp;
	int64_t period_ns;	/* between probes */
	uint16_t size;		/* frame size, 0: minimum */
	int64_t report_ns;	/* between stat reports */
	int64_t timeout_ns;	/* loss timeout */

	int argc;
	char **argv;		/* positional arguments */
};

int cmd_sink(struct opts *opts);
int cmd_stat(struct opts *opts);

#endif

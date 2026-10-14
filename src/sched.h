#ifndef PTPING_SCHED_H
#define PTPING_SCHED_H

#include <stdint.h>
#include <time.h>

#include "ts.h"

/* When is the next probe due? A fixed rate, optionally limited to a
 * window of a cyclic schedule such as an 802.1Qbv gate. Due times
 * step by exact integer periods, so scheduling latency does not
 * accumulate into the rate; a due time outside the window moves to
 * the start of the next one.
 *
 * A plain rate runs on CLOCK_MONOTONIC. A window, or a launch time,
 * is in CLOCK_TAI, the PHC's timescale, and runs on CLOCK_REALTIME
 * with the TAI offset applied, since timerfd cannot do TAI. */

struct sched_window {
	int64_t cycle_ns;	/* 0: no window, send at the rate */
	int64_t base;		/* CLOCK_TAI, cycles start here */
	int64_t offset_ns;	/* window start within the cycle */
	int64_t len_ns;
};

struct sched {
	struct sched_window win;
	int64_t period_ns;
	int64_t txtime_lead_ns;	/* 0: no SO_TXTIME; else how early to send */
	int64_t next;		/* in the schedule's clock */
	int64_t due;		/* of the probe armed last */

	int fd;			/* timerfd, readable when a probe is due */
	clockid_t clock;	/* of the timerfd */
	int64_t tai_offset;	/* CLOCK_TAI minus CLOCK_REALTIME */
	int qos_fd;		/* keeps the CPUs out of deep idle states */
};

/* Start now, with a timerfd of the right clock. win may be NULL for
 * a plain rate. Returns -1 with errno set if win is inconsistent
 * (EINVAL) or the timerfd could not be created. */
int sched_init(struct sched *s, int64_t period_ns,
	       const struct sched_window *win, int64_t txtime_lead_ns);
void sched_fini(struct sched *s);

/* Due time of the next probe, in the schedule's clock. */
int64_t sched_next(struct sched *s);

/* Arm the timerfd for the next probe and remember its due time in
 * s->due. */
void sched_arm(struct sched *s);

/* Launch time for the probe armed last, or TS_NONE if it is to be
 * sent as soon as the timer fires. */
int64_t sched_txtime(const struct sched *s);

/* Where in the cycle a timestamp falls: 0 <= phase < cycle, or
 * TS_NONE without a window or a timestamp. Hardware timestamps are in
 * the schedule's clock already; software ones are in CLOCK_REALTIME. */
int64_t sched_phase(const struct sched *s, int64_t ts, enum ts_mode mode);

/* Whether a phase lies inside the window. */
int sched_in_window(const struct sched *s, int64_t phase);

#endif

#ifndef TAPING_SCHED_H
#define TAPING_SCHED_H

#include <stdint.h>

/* When is the next probe due? Today: a fixed rate. Due times step by
 * exact integer periods from the start, so the rate does not drift
 * with scheduling latency. */
struct sched {
	int64_t next;		/* in whatever clock start was given in */
	int64_t period_ns;
};

static inline void sched_init(struct sched *s, int64_t start, int64_t period_ns)
{
	s->next = start;
	s->period_ns = period_ns;
}

/* Due time of the next probe; calling it again gives the one after. */
static inline int64_t sched_next(struct sched *s)
{
	int64_t due = s->next;

	s->next += s->period_ns;
	return due;
}

#endif

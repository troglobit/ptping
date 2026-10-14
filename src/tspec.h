#ifndef PTPING_TSPEC_H
#define PTPING_TSPEC_H

#include <stdint.h>
#include <time.h>

#define NS_PER_S  INT64_C(1000000000)
#define NS_PER_MS INT64_C(1000000)
#define NS_PER_US INT64_C(1000)

/* Timestamps are int64_t nanoseconds throughout; TS_NONE marks a
 * timestamp that was never taken. */
#define TS_NONE INT64_MIN

static inline int64_t ts_from_timespec(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * NS_PER_S + ts->tv_nsec;
}

static inline struct timespec ts_to_timespec(int64_t ns)
{
	return (struct timespec){
		.tv_sec = ns / NS_PER_S,
		.tv_nsec = ns % NS_PER_S,
	};
}

static inline int64_t ts_now(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return ts_from_timespec(&ts);
}

#endif

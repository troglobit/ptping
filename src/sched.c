#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "log.h"
#include "sched.h"
#include "tspec.h"

static int64_t floor_mod(int64_t a, int64_t m)
{
	int64_t r = a % m;

	return r < 0 ? r + m : r;
}

int sched_in_window(const struct sched *s, int64_t phase)
{
	return phase >= s->win.offset_ns &&
		phase < s->win.offset_ns + s->win.len_ns;
}

/* Waking from a deep idle state takes longer than a window test
 * tolerates: over 100 us on common CPUs. A PM QoS request of zero
 * latency keeps the CPUs out of them for as long as the file is
 * open. Returns the file descriptor, or -1. */
static int qos_open(void)
{
	int32_t zero = 0;
	int fd;

	fd = open("/dev/cpu_dma_latency", O_RDWR | O_CLOEXEC);
	if (fd < 0 || write(fd, &zero, sizeof(zero)) != sizeof(zero)) {
		warn("cannot keep the CPUs out of deep idle states (%m); "
		     "probes may leave over 100 us late");
		if (fd >= 0)
			close(fd);
		return -1;
	}

	return fd;
}

int sched_init(struct sched *s, int64_t period_ns,
	       const struct sched_window *win, int64_t txtime_lead_ns)
{
	*s = (struct sched){
		.period_ns = period_ns,
		.txtime_lead_ns = txtime_lead_ns,
		.clock = CLOCK_MONOTONIC,
		.fd = -1,
		.qos_fd = -1,
	};
	if (win)
		s->win = *win;

	errno = EINVAL;
	if (s->win.cycle_ns < 0 || s->win.offset_ns < 0 || s->win.len_ns < 0 ||
	    txtime_lead_ns < 0)
		return -1;
	if (s->win.cycle_ns &&
	    (!s->win.len_ns || s->win.offset_ns + s->win.len_ns > s->win.cycle_ns))
		return -1;

	if (s->win.cycle_ns || txtime_lead_ns) {
		s->clock = CLOCK_REALTIME;
		s->tai_offset = ts_now(CLOCK_TAI) - ts_now(CLOCK_REALTIME);
		if (!s->tai_offset)
			warn("CLOCK_TAI is not offset from CLOCK_REALTIME; "
			     "is the UTC offset set?");

		/* The default timer slack is 50 us, which is most of the
		 * placement error a window test cares about. */
		prctl(PR_SET_TIMERSLACK, 1);
		s->qos_fd = qos_open();
	}

	s->fd = timerfd_create(s->clock, TFD_NONBLOCK);
	if (s->fd < 0)
		return -1;

	s->next = ts_now(s->clock) + s->tai_offset;
	return 0;
}

void sched_fini(struct sched *s)
{
	if (s->fd >= 0)
		close(s->fd);
	if (s->qos_fd >= 0)
		close(s->qos_fd);
	s->fd = s->qos_fd = -1;
}

/* The first time at or after t that lies inside the window. */
static int64_t align(const struct sched *s, int64_t t)
{
	const struct sched_window *w = &s->win;
	int64_t phase = sched_phase(s, t, TS_MODE_HW);

	if (sched_in_window(s, phase))
		return t;
	if (phase < w->offset_ns)
		return t + (w->offset_ns - phase);
	return t + (w->cycle_ns - phase) + w->offset_ns;
}

int64_t sched_next(struct sched *s)
{
	int64_t due = s->next;

	if (s->win.cycle_ns)
		due = align(s, due);

	s->next = due + s->period_ns;
	return due;
}

void sched_arm(struct sched *s)
{
	struct itimerspec its;

	s->due = sched_next(s);
	its = (struct itimerspec){
		.it_value = ts_to_timespec(s->due - s->tai_offset - s->txtime_lead_ns),
	};

	if (timerfd_settime(s->fd, TFD_TIMER_ABSTIME, &its, NULL))
		err_errno("timerfd_settime");
}

int64_t sched_txtime(const struct sched *s)
{
	return s->txtime_lead_ns ? s->due : TS_NONE;
}

int64_t sched_phase(const struct sched *s, int64_t ts, enum ts_mode mode)
{
	if (!s->win.cycle_ns || ts == TS_NONE)
		return TS_NONE;
	if (mode == TS_MODE_SW)
		ts += s->tai_offset;

	return floor_mod(ts - s->win.base, s->win.cycle_ns);
}

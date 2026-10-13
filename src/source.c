#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "log.h"
#include "ptp.h"
#include "sched.h"
#include "source.h"
#include "ts.h"
#include "tspec.h"

#define WELCOME_TIMEOUT_MS 5000

static volatile sig_atomic_t interrupts;

static void on_signal(int sig)
{
	interrupts++;
}

static void on_sample(void *arg, const struct flow_sample *s)
{
	struct source_flow *sf = arg;

	if (sf->src->ops->sample)
		sf->src->ops->sample(sf->src->priv, sf->which, s);
}

static int flows_init(struct source *src)
{
	struct opts *opts = src->opts;
	uint64_t window = opts->timeout_ns / opts->period_ns * 2 + 64;
	unsigned i;

	for (i = 0; i < 2; i++) {
		src->flow[i].src = src;
		src->flow[i].which = i;
		if (flow_init(&src->flow[i].flow, opts->timeout_ns, window,
			      on_sample, &src->flow[i]))
			return -1;
	}

	return 0;
}

/* The PTP message length that gives the requested frame size. */
static int probe_len(struct source *src)
{
	unsigned overhead = 14 + (src->sink.ss_family == AF_INET ? 20 : 40) + 8 + 4;
	unsigned size = src->opts->size;

	if (!size)
		return PTP_SYNC_LEN;
	if (size < overhead + PTP_SYNC_LEN) {
		err("--size: the smallest frame is %u bytes", overhead + PTP_SYNC_LEN);
		return -1;
	}

	return size - overhead;
}

static int probe_open(struct source *src, struct sockaddr_storage *local,
		      socklen_t llen)
{
	struct opts *opts = src->opts;
	int fd, len, ret;

	len = probe_len(src);
	if (len < 0)
		return -1;

	src->pkt = malloc(len);
	src->pkt_len = len;
	if (!src->pkt || ptp_sync_build(src->pkt, len, &src->self, opts->domain, 0)) {
		err("--size: unsupported; must be the minimum, or at least four "
		    "bytes larger and even");
		return -1;
	}

	fd = socket(local->ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		err_errno("socket");
		return -1;
	}
	src->probe_fd = fd;

	addr_set_port(local, opts->port);
	ret = bind(fd, (struct sockaddr *)local, llen);
	if (ret && errno == EADDRINUSE) {
		if (opts->bidir) {
			err("port %u is in use, and --bidir needs it to receive "
			    "the reverse probes", opts->port);
			return -1;
		}
		warn("port %u is in use, sending from an ephemeral port", opts->port);
		addr_set_port(local, 0);
		ret = bind(fd, (struct sockaddr *)local, llen);
	}
	if (ret) {
		err_errno("bind");
		return -1;
	}

	if (sock_set_dscp(fd, local->ss_family, opts->dscp) ||
	    ts_setup_sock(fd, src->mode)) {
		err_errno("probe socket");
		return -1;
	}

	return 0;
}

static int hello(struct source *src)
{
	struct opts *opts = src->opts;
	struct ctrl_msg msg = {
		.type = CTRL_HELLO,
		.hello = {
			.flags = opts->bidir ? CTRL_HELLO_BIDIR : 0,
			.domain = opts->domain,
			.tstamp = opts->ts_mode,
			.src = src->self,
			.rev = {
				.period_ns = opts->period_ns,
				.count = opts->count,
				.size = src->pkt_len,
				.dscp = opts->dscp,
			},
		},
	};
	struct pollfd pfd = { .fd = src->ctrl.fd, .events = POLLIN };
	int n;

	if (ctrl_send(&src->ctrl, &msg)) {
		err_errno("control connection");
		return -1;
	}

	while ((n = ctrl_recv(&src->ctrl, &msg)) == 0) {
		if (poll(&pfd, 1, WELCOME_TIMEOUT_MS) <= 0) {
			err("no answer from sink");
			return -1;
		}
	}
	if (n < 0) {
		err("sink closed the connection before answering");
		return -1;
	}
	if (msg.type == CTRL_ERROR) {
		err("sink: %s", msg.text);
		return -1;
	}
	if (msg.type != CTRL_WELCOME) {
		err("unexpected message %u from sink", msg.type);
		return -1;
	}
	if (msg.welcome.status) {
		err("sink refused: %s", msg.welcome.text);
		return -1;
	}
	/* The sink refuses a mismatch unless we said any. */
	if ((int)msg.welcome.tstamp != src->mode)
		warn("sink uses %s timestamps, this end %s; latencies will be off",
		     ts_mode_str(msg.welcome.tstamp), ts_mode_str(src->mode));

	src->sink_id = msg.welcome.src;
	return 0;
}

static int session_open(struct source *src)
{
	struct opts *opts = src->opts;
	struct sockaddr_storage local;
	socklen_t llen = sizeof(local);
	int fd, mode;

	if (addr_resolve(opts->argv[0], opts->port, &src->sink, &src->sink_len))
		return -1;

	fd = socket(src->sink.ss_family, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&src->sink, src->sink_len)) {
		err_errno("%s", opts->argv[0]);
		return -1;
	}
	ctrl_conn_init(&src->ctrl, fd);

	if (getsockname(fd, (struct sockaddr *)&local, &llen) ||
	    netif_to_remote((struct sockaddr *)&src->sink, src->sink_len, &src->ni)) {
		err_errno("cannot find the interface towards %s", opts->argv[0]);
		return -1;
	}

	mode = ts_setup_netif(src->ni.name, opts->ts_mode);
	if (mode < 0) {
		err("no hardware timestamping on %s", src->ni.name);
		return -1;
	}
	src->mode = mode;
	ptp_port_id_self(&src->self, src->ni.mac);

	if (probe_open(src, &local, llen) || hello(src))
		return -1;

	info("%s timestamps on %s", ts_mode_str(src->mode), src->ni.name);
	return 0;
}

static void timer_ack(int fd)
{
	uint64_t expirations;

	(void)!read(fd, &expirations, sizeof(expirations));
}

static void tx_stop(struct source *src)
{
	struct itimerspec its = { 0 };

	timerfd_settime(src->tx_timer, 0, &its, NULL);
	src->stop_at = ts_now(CLOCK_MONOTONIC) + src->opts->timeout_ns;
}

static void tx_probe(struct source *src, struct sched *sched)
{
	ptp_sync_set_seq(src->pkt, src->seq);
	if (sendto(src->probe_fd, src->pkt, src->pkt_len, 0,
		   (struct sockaddr *)&src->sink, src->sink_len) < 0 &&
	    errno != EAGAIN)
		dbg("probe %llu: %m", (unsigned long long)src->seq);
	flow_add(&src->flow[CTRL_FLOW_FWD].flow, src->seq, ts_now(CLOCK_MONOTONIC));
	src->seq++;

	if (src->opts->count && src->seq == src->opts->count)
		tx_stop(src);
	else
		sched_arm(sched, src->tx_timer);
}

static void tx_timestamps(struct source *src)
{
	int64_t now = ts_now(CLOCK_MONOTONIC), ts;
	uint32_t id;

	while (ts_recv_tx(src->probe_fd, &id, &ts) > 0) {
		src->tx_seq = ts_id_extend(src->tx_seq, id);
		flow_tx(&src->flow[CTRL_FLOW_FWD].flow, src->tx_seq, ts, now);
	}
}

static void rx_probes(struct source *src)
{
	int64_t now = ts_now(CLOCK_MONOTONIC), rx_ts;
	struct ptp_sync sync;

	while (ptp_recv_sync(src->probe_fd, &sync, &rx_ts) > 0) {
		if (sync.domain != src->opts->domain ||
		    ptp_port_id_cmp(&sync.src, &src->sink_id))
			continue;

		src->rev_seq = ptp_seq_extend(src->rev_seq, sync.seq);
		flow_rx(&src->flow[CTRL_FLOW_REV].flow, src->rev_seq, rx_ts,
			sync.correction, now);
	}
}

/* Returns -1 when the control connection is done. */
static int rx_ctrl(struct source *src)
{
	int64_t now = ts_now(CLOCK_MONOTONIC);
	struct ctrl_msg msg;
	struct flow *f;
	unsigned i;
	int n;

	while ((n = ctrl_recv(&src->ctrl, &msg)) > 0) {
		switch (msg.type) {
		case CTRL_TS:
			if (msg.ts.flow > CTRL_FLOW_REV)
				break;
			f = &src->flow[msg.ts.flow].flow;
			for (i = 0; i < msg.ts.count; i++) {
				if (msg.ts.kind == CTRL_KIND_RX)
					flow_rx(f, msg.ts.rec[i].seq, msg.ts.rec[i].ts,
						msg.ts.rec[i].correction, now);
				else
					flow_tx(f, msg.ts.rec[i].seq, msg.ts.rec[i].ts, now);
			}
			break;
		case CTRL_ERROR:
			err("sink: %s", msg.text);
			return -1;
		case CTRL_BYE:
			return -1;
		default:
			break;
		}
	}

	if (n < 0) {
		if (errno)
			err_errno("control connection");
		else
			err("sink went away");
		return -1;
	}

	return 0;
}

static void sweep(struct source *src, int64_t now)
{
	unsigned i;

	for (i = 0; i < 2; i++)
		flow_sweep(&src->flow[i].flow, now);
}

static void report(struct source *src, int final)
{
	if (src->ops->report)
		src->ops->report(src->priv, src, final);
}

static int drained(struct source *src, int64_t now)
{
	if (!src->stop_at)
		return 0;
	if (now >= src->stop_at)
		return 1;

	/* With a finite count, both ends stop by themselves and there
	 * is no point in waiting once every sample is in. */
	return src->opts->count &&
		!flow_outstanding(&src->flow[CTRL_FLOW_FWD].flow) &&
		!flow_outstanding(&src->flow[CTRL_FLOW_REV].flow);
}

static int loop(struct source *src)
{
	struct pollfd pfd[] = {
		{ .fd = src->ctrl.fd, .events = POLLIN },
		{ .fd = src->probe_fd, .events = POLLIN },
		{ .fd = src->tx_timer, .events = POLLIN },
		{ .fd = src->report_timer, .events = POLLIN },
	};
	struct itimerspec its = {
		.it_value = ts_to_timespec(src->opts->report_ns),
		.it_interval = ts_to_timespec(src->opts->report_ns),
	};
	struct sched sched;
	int64_t now;
	int ret = 0, timeout;

	src->started = ts_now(CLOCK_MONOTONIC);
	sched_init(&sched, src->started, src->opts->period_ns);
	sched_arm(&sched, src->tx_timer);
	timerfd_settime(src->report_timer, 0, &its, NULL);

	for (;;) {
		now = ts_now(CLOCK_MONOTONIC);
		sweep(src, now);
		if (drained(src, now) || interrupts > 1)
			break;
		if (interrupts && !src->stop_at)
			tx_stop(src);

		timeout = -1;
		if (src->stop_at)
			timeout = (src->stop_at - now) / NS_PER_MS + 1;
		if (poll(pfd, 4, timeout) < 0)
			continue;

		if (pfd[0].revents && rx_ctrl(src)) {
			ret = 1;
			break;
		}
		if (pfd[1].revents & POLLERR)
			tx_timestamps(src);
		if (pfd[1].revents & POLLIN)
			rx_probes(src);
		if (pfd[2].revents) {
			timer_ack(src->tx_timer);
			tx_probe(src, &sched);
		}
		if (pfd[3].revents) {
			timer_ack(src->report_timer);
			report(src, 0);
		}
	}

	return ret;
}

int source_run(struct source *src, struct opts *opts,
	       const struct source_ops *ops, void *priv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct ctrl_msg bye = { .type = CTRL_BYE };
	unsigned i;
	int ret = 1;

	*src = (struct source){
		.opts = opts,
		.ops = ops,
		.priv = priv,
		.probe_fd = -1,
		.tx_timer = -1,
		.report_timer = -1,
		.ctrl.fd = -1,
	};

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	src->tx_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	src->report_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (src->tx_timer < 0 || src->report_timer < 0) {
		err_errno("timerfd");
		goto out;
	}

	if (flows_init(src) || session_open(src))
		goto out;

	ret = loop(src);
	ctrl_send(&src->ctrl, &bye);
	sweep(src, INT64_MAX);
	report(src, 1);
out:
	for (i = 0; i < 2; i++)
		flow_fini(&src->flow[i].flow);
	free(src->pkt);
	close(src->probe_fd);
	close(src->tx_timer);
	close(src->report_timer);
	close(src->ctrl.fd);
	return ret;
}

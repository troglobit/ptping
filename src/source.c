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
	struct stream_flow *sf = arg;
	struct source *src = sf->st->src;

	if (src->ops->sample)
		src->ops->sample(src->priv, sf->st, sf->which, s);
}

static int flows_init(struct stream *st)
{
	const struct stream_opts *opts = st->opts;
	uint64_t window = opts->timeout_ns / opts->period_ns * 2 + 64;
	unsigned i;

	for (i = 0; i < 2; i++) {
		st->flow[i].st = st;
		st->flow[i].which = i;
		if (flow_init(&st->flow[i].flow, opts->timeout_ns, window,
			      on_sample, &st->flow[i]))
			return -1;
	}

	return 0;
}

/* The PTP message length that gives the requested frame size. */
static int probe_len(struct stream *st)
{
	unsigned overhead = 14 + (st->sink.ss_family == AF_INET ? 20 : 40) + 8 + 4;
	unsigned size = st->opts->size;

	if (!size)
		return PTP_SYNC_LEN;
	if (size < overhead + PTP_SYNC_LEN) {
		err("%s: --size: the smallest frame is %u bytes", st->opts->name,
		    overhead + PTP_SYNC_LEN);
		return -1;
	}

	return size - overhead;
}

/* Probes leave from an ephemeral port, so that only a source with
 * --bidir needs the probe port. The socket sends nothing but this
 * stream's probes, so its transmission counter is the sequence
 * number. */
static int tx_open(struct stream *st, struct sockaddr_storage local,
		   socklen_t llen)
{
	const struct stream_opts *opts = st->opts;
	int fd, len;

	len = probe_len(st);
	if (len < 0)
		return -1;

	st->pkt = malloc(len);
	st->pkt_len = len;
	if (!st->pkt || ptp_sync_build(st->pkt, len, &st->self, opts->domain, 0)) {
		err("%s: --size: unsupported; must be the minimum, or at least "
		    "four bytes larger and even", opts->name);
		return -1;
	}

	fd = socket(local.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		err_errno("socket");
		return -1;
	}
	st->tx_fd = fd;

	addr_set_port(&local, 0);
	if (bind(fd, (struct sockaddr *)&local, llen) ||
	    sock_set_dscp(fd, local.ss_family, opts->dscp) ||
	    ts_setup_sock(fd, st->mode)) {
		err_errno("%s: probe socket", opts->name);
		return -1;
	}
	if (opts->txtime_lead_ns && ts_setup_txtime(fd)) {
		err_errno("%s: --txtime: SO_TXTIME (needs CAP_NET_ADMIN)", opts->name);
		return -1;
	}

	return 0;
}

/* Reverse probes arrive on the probe port, where hardware recognizes
 * them, and the streams from one local address share a socket there. */
static int rx_open(struct stream *st, struct sockaddr_storage local,
		   socklen_t llen)
{
	struct source *src = st->src;
	struct source_rx *rx;
	unsigned i;
	int fd;

	addr_set_port(&local, src->opts->port);
	for (i = 0; i < src->nrx; i++) {
		if (!memcmp(&src->rx[i].addr, &local, sizeof(local)))
			return 0;
	}

	fd = socket(local.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		err_errno("socket");
		return -1;
	}
	rx = &src->rx[src->nrx++];
	rx->addr = local;
	rx->fd = fd;

	if (bind(fd, (struct sockaddr *)&local, llen)) {
		if (errno == EADDRINUSE)
			err("%s: port %u is in use, and --bidir needs it to "
			    "receive the reverse probes", st->opts->name,
			    src->opts->port);
		else
			err_errno("%s: bind", st->opts->name);
		return -1;
	}
	/* Streams of either mode share this socket. */
	if (ts_setup_sock(fd, TS_MODE_ANY)) {
		err_errno("%s: SO_TIMESTAMPING", st->opts->name);
		return -1;
	}

	return 0;
}

static int hello(struct stream *st)
{
	const struct stream_opts *opts = st->opts;
	struct ctrl_msg msg = {
		.type = CTRL_HELLO,
		.hello = {
			.flags = opts->bidir ? CTRL_HELLO_BIDIR : 0,
			.domain = opts->domain,
			.tstamp = st->mode,
			.src = st->self,
			.rev = {
				.period_ns = opts->period_ns,
				.count = opts->count,
				.size = st->pkt_len,
				.dscp = opts->dscp,
				.win = opts->win,
				.txtime_lead_ns = opts->txtime_lead_ns,
			},
		},
	};
	struct pollfd pfd = { .fd = st->ctrl.fd, .events = POLLIN };
	int n;

	if (ctrl_send(&st->ctrl, &msg)) {
		err_errno("%s: control connection", opts->name);
		return -1;
	}

	while ((n = ctrl_recv(&st->ctrl, &msg)) == 0) {
		if (poll(&pfd, 1, WELCOME_TIMEOUT_MS) <= 0) {
			err("%s: no answer from sink", opts->name);
			return -1;
		}
	}
	if (n < 0) {
		err("%s: sink closed the connection before answering", opts->name);
		return -1;
	}
	if (msg.type == CTRL_ERROR) {
		err("%s: sink: %s", opts->name, msg.text);
		return -1;
	}
	if (msg.type != CTRL_WELCOME) {
		err("%s: unexpected message %u from sink", opts->name, msg.type);
		return -1;
	}
	if (msg.welcome.status) {
		err("%s: sink refused: %s", opts->name, msg.welcome.text);
		return -1;
	}
	st->sink_id = msg.welcome.src;
	return 0;
}

static int session_open(struct stream *st)
{
	const struct stream_opts *opts = st->opts;
	struct sockaddr_storage local = { 0 };
	socklen_t llen = sizeof(local);
	int fd, mode;

	if (sched_init(&st->sched, opts->period_ns, &opts->win,
		       opts->txtime_lead_ns)) {
		err_errno("%s: schedule", opts->name);
		return -1;
	}

	if (addr_resolve(opts->sink, st->src->opts->port, &st->sink, &st->sink_len))
		return -1;

	fd = socket(st->sink.ss_family, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&st->sink, st->sink_len)) {
		err_errno("%s", opts->sink);
		return -1;
	}
	ctrl_conn_init(&st->ctrl, fd);

	if (getsockname(fd, (struct sockaddr *)&local, &llen) ||
	    netif_to_remote((struct sockaddr *)&st->sink, st->sink_len, &st->ni)) {
		err_errno("cannot find the interface towards %s", opts->sink);
		return -1;
	}

	mode = ts_setup_netif(st->ni.name, opts->ts_mode);
	if (mode < 0) {
		err("%s: no hardware timestamping on %s", opts->name, st->ni.name);
		return -1;
	}
	st->mode = mode;
	ptp_port_id_self(&st->self, st->ni.mac, st->index);

	if (tx_open(st, local, llen) ||
	    (opts->bidir && rx_open(st, local, llen)) ||
	    hello(st))
		return -1;

	info("%s: %s timestamps on %s", opts->name, ts_mode_str(st->mode),
	     st->ni.name);
	return 0;
}

static void timer_ack(int fd)
{
	uint64_t expirations;

	(void)!read(fd, &expirations, sizeof(expirations));
}

static void tx_stop(struct stream *st)
{
	struct itimerspec its = { 0 };

	timerfd_settime(st->sched.fd, 0, &its, NULL);
	st->stop_at = ts_now(CLOCK_MONOTONIC) + st->opts->timeout_ns;
}

static void tx_probe(struct stream *st)
{
	ptp_sync_set_seq(st->pkt, st->seq);
	if (ts_sendto(st->tx_fd, st->pkt, st->pkt_len,
		      (struct sockaddr *)&st->sink, st->sink_len,
		      sched_txtime(&st->sched)) < 0 && errno != EAGAIN)
		dbg("%s: probe %llu: %m", st->opts->name, (unsigned long long)st->seq);
	flow_add(&st->flow[CTRL_FLOW_FWD].flow, st->seq, ts_now(CLOCK_MONOTONIC));
	st->seq++;

	if (st->opts->count && st->seq == st->opts->count)
		tx_stop(st);
	else
		sched_arm(&st->sched);
}

static void tx_timestamps(struct stream *st)
{
	int64_t now = ts_now(CLOCK_MONOTONIC), ts;
	uint32_t id;

	while (ts_recv_tx(st->tx_fd, st->mode, &id, &ts) > 0) {
		st->tx_seq = ts_id_extend(st->tx_seq, id);
		flow_tx(&st->flow[CTRL_FLOW_FWD].flow, st->tx_seq, ts, now);
	}
}

/* Reverse probes carry the identity the sink gave their session. */
static struct stream *rx_stream(struct source *src, const struct ptp_sync *sync)
{
	struct stream *st;
	unsigned i;

	for (i = 0; i < src->opts->nstreams; i++) {
		st = &src->streams[i];
		if (st->opts->bidir && sync->domain == st->opts->domain &&
		    !ptp_port_id_cmp(&sync->src, &st->sink_id))
			return st;
	}

	return NULL;
}

static void rx_probes(struct source *src, int fd)
{
	int64_t now = ts_now(CLOCK_MONOTONIC);
	struct ptp_sync sync;
	struct stream *st;
	struct ts_pair rx;

	while (ptp_recv_sync(fd, &sync, &rx) > 0) {
		st = rx_stream(src, &sync);
		if (!st)
			continue;

		st->rev_seq = ptp_seq_extend(st->rev_seq, sync.seq);
		flow_rx(&st->flow[CTRL_FLOW_REV].flow, st->rev_seq,
			ts_pick(&rx, st->mode), sync.correction, now);
	}
}

/* Returns -1 when the control connection is done. */
static int rx_ctrl(struct stream *st)
{
	int64_t now = ts_now(CLOCK_MONOTONIC);
	struct ctrl_msg msg;
	struct flow *f;
	unsigned i;
	int n;

	while ((n = ctrl_recv(&st->ctrl, &msg)) > 0) {
		switch (msg.type) {
		case CTRL_TS:
			if (msg.ts.flow > CTRL_FLOW_REV)
				break;
			f = &st->flow[msg.ts.flow].flow;
			for (i = 0; i < msg.ts.count; i++) {
				if (msg.ts.kind == CTRL_KIND_RX)
					flow_rx(f, msg.ts.rec[i].seq, msg.ts.rec[i].ts,
						msg.ts.rec[i].correction, now);
				else
					flow_tx(f, msg.ts.rec[i].seq, msg.ts.rec[i].ts, now);
			}
			break;
		case CTRL_ERROR:
			err("%s: sink: %s", st->opts->name, msg.text);
			return -1;
		case CTRL_BYE:
			return -1;
		default:
			break;
		}
	}

	if (n < 0) {
		if (errno)
			err_errno("%s: control connection", st->opts->name);
		else
			err("%s: sink went away", st->opts->name);
		return -1;
	}

	return 0;
}

static void sweep(struct source *src, int64_t now)
{
	unsigned i, j;

	for (i = 0; i < src->opts->nstreams; i++) {
		for (j = 0; j < 2; j++)
			flow_sweep(&src->streams[i].flow[j].flow, now);
	}
}

static void report(struct source *src, int final)
{
	if (src->ops->report)
		src->ops->report(src->priv, src, final);
}

static int drained(struct stream *st, int64_t now)
{
	if (st->lost)
		return 1;
	if (!st->stop_at)
		return 0;
	if (now >= st->stop_at)
		return 1;

	/* With a finite count, both ends stop by themselves and there
	 * is no point in waiting once every sample is in. */
	return st->opts->count &&
		!flow_outstanding(&st->flow[CTRL_FLOW_FWD].flow) &&
		!flow_outstanding(&st->flow[CTRL_FLOW_REV].flow);
}

/* The poll set is the report timer, the rx sockets, and then three
 * descriptors per stream. Returns its size. */
static unsigned poll_init(struct source *src, struct pollfd *pfd)
{
	struct pollfd *p = pfd;
	struct stream *st;
	unsigned i;

	*p++ = (struct pollfd){ .fd = src->report_timer, .events = POLLIN };
	for (i = 0; i < src->nrx; i++)
		*p++ = (struct pollfd){ .fd = src->rx[i].fd, .events = POLLIN };
	for (i = 0; i < src->opts->nstreams; i++) {
		st = &src->streams[i];
		*p++ = (struct pollfd){ .fd = st->ctrl.fd, .events = POLLIN };
		*p++ = (struct pollfd){ .fd = st->tx_fd, .events = POLLERR };
		*p++ = (struct pollfd){ .fd = st->sched.fd, .events = POLLIN };
	}

	return p - pfd;
}

static int loop(struct source *src)
{
	struct itimerspec its = {
		.it_value = ts_to_timespec(src->opts->report_ns),
		.it_interval = ts_to_timespec(src->opts->report_ns),
	};
	struct pollfd pfd[1 + 4 * PTP_SELF_PORTS], *p;
	unsigned i, n = poll_init(src, pfd);
	int ret = 0, timeout, done;
	struct stream *st;
	int64_t now, ms;

	src->started = ts_now(CLOCK_MONOTONIC);
	for (i = 0; i < src->opts->nstreams; i++)
		sched_arm(&src->streams[i].sched);
	timerfd_settime(src->report_timer, 0, &its, NULL);

	for (;;) {
		now = ts_now(CLOCK_MONOTONIC);
		sweep(src, now);

		done = 1;
		timeout = -1;
		for (i = 0; i < src->opts->nstreams; i++) {
			st = &src->streams[i];
			if (drained(st, now))
				continue;
			done = 0;

			if (interrupts && !st->stop_at)
				tx_stop(st);

			ms = (st->stop_at - now) / NS_PER_MS + 1;
			if (st->stop_at && (timeout < 0 || ms < timeout))
				timeout = ms;
		}
		if (done || interrupts > 1)
			break;

		if (poll(pfd, n, timeout) < 0)
			continue;

		if (pfd[0].revents) {
			timer_ack(src->report_timer);
			report(src, 0);
		}
		for (i = 0; i < src->nrx; i++) {
			if (pfd[1 + i].revents)
				rx_probes(src, src->rx[i].fd);
		}

		for (i = 0, p = pfd + 1 + src->nrx; i < src->opts->nstreams; i++, p += 3) {
			st = &src->streams[i];
			if (p[0].revents && rx_ctrl(st)) {
				st->lost = 1;
				p[0].fd = p[1].fd = p[2].fd = -1;
				ret = 1;
				continue;
			}
			if (p[1].revents & POLLERR)
				tx_timestamps(st);
			if (p[2].revents) {
				timer_ack(st->sched.fd);
				tx_probe(st);
			}
		}
	}

	return ret;
}

int source_run(struct opts *opts, const struct source_ops *ops, void *priv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	struct ctrl_msg bye = { .type = CTRL_BYE };
	struct source run, *src = &run;
	struct stream *st;
	unsigned i, j;
	int ret = 1;

	*src = (struct source){
		.opts = opts,
		.ops = ops,
		.priv = priv,
		.report_timer = -1,
	};

	src->streams = calloc(opts->nstreams, sizeof(*src->streams));
	if (!src->streams) {
		err_errno("streams");
		return 1;
	}
	for (i = 0; i < opts->nstreams; i++) {
		src->streams[i] = (struct stream){
			.src = src,
			.opts = &opts->streams[i],
			.index = i,
			.tx_fd = -1,
			.sched = { .fd = -1, .qos_fd = -1 },
			.ctrl.fd = -1,
		};
	}

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	src->report_timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (src->report_timer < 0) {
		err_errno("timerfd");
		goto out;
	}

	for (i = 0; i < opts->nstreams; i++) {
		st = &src->streams[i];
		if (flows_init(st) || session_open(st))
			goto out;
	}

	ret = loop(src);
	for (i = 0; i < opts->nstreams; i++) {
		if (!src->streams[i].lost)
			ctrl_send(&src->streams[i].ctrl, &bye);
	}
	sweep(src, INT64_MAX);
	report(src, 1);
out:
	for (i = 0; i < opts->nstreams; i++) {
		st = &src->streams[i];
		for (j = 0; j < 2; j++)
			flow_fini(&st->flow[j].flow);
		free(st->pkt);
		sched_fini(&st->sched);
		close(st->tx_fd);
		close(st->ctrl.fd);
	}
	for (i = 0; i < src->nrx; i++)
		close(src->rx[i].fd);
	close(src->report_timer);
	free(src->streams);
	return ret;
}

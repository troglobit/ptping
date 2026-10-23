#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "ctrl.h"
#include "log.h"
#include "netif.h"
#include "ptp.h"
#include "ptping.h"
#include "sched.h"
#include "ts.h"
#include "tspec.h"

#define MAX_SESSIONS PTP_SELF_PORTS
#define BATCH_DELAY_NS (50 * NS_PER_MS)
#define HELLO_TIMEOUT_NS (5 * NS_PER_S)

struct session {
	int used;
	struct ctrl_conn ctrl;
	int hello;		/* HELLO received */
	int64_t due;		/* HELLO deadline, then batch deadline; 0: none */

	struct ptp_port_id src;	/* on forward probes */
	struct ptp_port_id self;	/* on reverse probes */
	uint8_t domain;
	enum ts_mode mode;
	uint64_t fwd_seq;	/* extension state */

	struct ctrl_msg batch[2];	/* CTRL_KIND_RX forward, CTRL_KIND_TX reverse */

	/* Reverse flow, when the source asked for one. */
	int rev_fd;
	struct sched sched;
	uint32_t rev_left;	/* probes still to send, 0: until BYE */
	uint64_t rev_seq;	/* next to send */
	uint64_t rev_tx_seq;	/* extension state for the tx counter */
	uint8_t *pkt;
	size_t pkt_len;
};

struct sink {
	struct opts *opts;
	int listen_fd, udp_fd;
	struct session sessions[MAX_SESSIONS];
};

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	stop = 1;
}

static int sock_err(const char *what)
{
	err_errno("%s", what);
	return -1;
}

static struct session *session_find(struct sink *sink, uint8_t domain,
				    const struct ptp_port_id *src)
{
	struct session *s;
	unsigned i;

	for (i = 0; i < MAX_SESSIONS; i++) {
		s = &sink->sessions[i];
		if (s->used && s->hello && s->domain == domain &&
		    !ptp_port_id_cmp(&s->src, src))
			return s;
	}

	return NULL;
}

static void session_close(struct session *s)
{
	if (!s->used)
		return;

	close(s->ctrl.fd);
	if (s->rev_fd >= 0)
		close(s->rev_fd);
	sched_fini(&s->sched);
	free(s->pkt);
	s->used = 0;
}

static void batch_flush(struct session *s)
{
	unsigned i;

	for (i = 0; i < 2; i++) {
		if (!s->batch[i].ts.count)
			continue;
		if (ctrl_send(&s->ctrl, &s->batch[i])) {
			err_errno("control connection");
			session_close(s);
			return;
		}
		s->batch[i].ts.count = 0;
	}

	s->due = 0;
}

static void batch_add(struct session *s, enum ctrl_kind kind, uint64_t seq,
		      int64_t ts, int64_t correction)
{
	struct ctrl_ts *b = &s->batch[kind].ts;

	b->rec[b->count++] = (struct ctrl_ts_rec){
		.seq = seq,
		.ts = ts,
		.correction = correction,
	};

	if (!s->due)
		s->due = ts_now(CLOCK_MONOTONIC) + BATCH_DELAY_NS;
	if (b->count == CTRL_TS_BATCH)
		batch_flush(s);
}

static void rev_send(struct session *s)
{
	uint64_t expirations;

	if (read(s->sched.fd, &expirations, sizeof(expirations)) < 0)
		return;

	ptp_sync_set_seq(s->pkt, s->rev_seq);
	if (ts_sendto(s->rev_fd, s->pkt, s->pkt_len, NULL, 0,
		      sched_txtime(&s->sched)) < 0 && errno != EAGAIN)
		dbg("reverse probe %llu: %m", (unsigned long long)s->rev_seq);
	s->rev_seq++;

	if (s->rev_left && !--s->rev_left)
		return;

	sched_arm(&s->sched);
}

static void rev_txts(struct session *s)
{
	uint32_t id;
	int64_t ts;

	while (ts_recv_tx(s->rev_fd, s->mode, &id, &ts) > 0) {
		s->rev_tx_seq = ts_id_extend(s->rev_tx_seq, id);
		batch_add(s, CTRL_KIND_TX, s->rev_tx_seq, ts, 0);
	}
}

static int rev_open(struct sink *sink, struct session *s,
		    const struct ctrl_hello *hello,
		    const struct sockaddr_storage *local, socklen_t llen,
		    char *reason)
{
	struct sockaddr_storage peer = { 0 }, bind_addr = *local;
	socklen_t plen = sizeof(peer);

	s->pkt = malloc(hello->rev.size);
	if (!s->pkt || ptp_sync_build(s->pkt, hello->rev.size, &s->self,
				      hello->domain, 0)) {
		sprintf(reason, "unsupported reverse probe size %u", hello->rev.size);
		return -1;
	}
	s->pkt_len = hello->rev.size;

	getpeername(s->ctrl.fd, (struct sockaddr *)&peer, &plen);
	addr_set_port(&peer, sink->opts->port);
	addr_set_port(&bind_addr, 0);

	s->rev_fd = socket(peer.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (s->rev_fd < 0 ||
	    bind(s->rev_fd, (struct sockaddr *)&bind_addr, llen) ||
	    connect(s->rev_fd, (struct sockaddr *)&peer, plen) ||
	    sock_set_dscp(s->rev_fd, peer.ss_family, hello->rev.dscp) ||
	    ts_setup_sock(s->rev_fd, s->mode)) {
		sprintf(reason, "reverse socket: %m");
		return -1;
	}
	if (hello->rev.txtime_lead_ns && ts_setup_txtime(s->rev_fd)) {
		sprintf(reason, "reverse socket: SO_TXTIME: %m");
		return -1;
	}

	if (sched_init(&s->sched, hello->rev.period_ns, &hello->rev.win,
		       hello->rev.txtime_lead_ns)) {
		sprintf(reason, "reverse schedule: %m");
		return -1;
	}

	s->rev_left = hello->rev.count;
	sched_arm(&s->sched);
	return 0;
}

/* Set up everything the HELLO asks for. On failure, reason says why,
 * for the WELCOME. */
static int session_start(struct sink *sink, struct session *s,
			 const struct ctrl_hello *hello, char *reason)
{
	struct sockaddr_storage local, peer;
	socklen_t llen = sizeof(local), plen = sizeof(peer);
	char astr[ADDR_STRLEN], pstr[PTP_PORT_ID_STRLEN];
	struct netif ni;

	if (session_find(sink, hello->domain, &hello->src)) {
		sprintf(reason, "%s is already connected",
			ptp_port_id_str(&hello->src, pstr));
		return -1;
	}

	if (getsockname(s->ctrl.fd, (struct sockaddr *)&local, &llen) ||
	    getpeername(s->ctrl.fd, (struct sockaddr *)&peer, &plen) ||
	    netif_to_remote((struct sockaddr *)&peer, plen, &ni)) {
		sprintf(reason, "no interface towards %s",
			addr_str((struct sockaddr *)&peer, astr));
		return -1;
	}

	/* The source picks the mode; this end delivers it or refuses. */
	if (ts_setup_netif(ni.name, hello->tstamp) < 0) {
		sprintf(reason, "no hardware timestamping on %s", ni.name);
		return -1;
	}

	s->mode = hello->tstamp;
	s->src = hello->src;
	s->domain = hello->domain;
	ptp_port_id_self(&s->self, ni.mac, s - sink->sessions);

	if ((hello->flags & CTRL_HELLO_BIDIR) &&
	    rev_open(sink, s, hello, &local, llen, reason))
		return -1;

	info("%s: session from %s on %s, %s timestamps%s",
	     addr_str((struct sockaddr *)&local, astr),
	     ptp_port_id_str(&hello->src, pstr), ni.name, ts_mode_str(s->mode),
	     hello->flags & CTRL_HELLO_BIDIR ? ", bidirectional" : "");
	return 0;
}

static void session_ctrl(struct sink *sink, struct session *s)
{
	struct ctrl_msg msg, welcome = { .type = CTRL_WELCOME };
	int n;

	while ((n = ctrl_recv(&s->ctrl, &msg)) > 0) {
		switch (msg.type) {
		case CTRL_HELLO:
			if (s->hello)
				break;
			if (session_start(sink, s, &msg.hello, welcome.welcome.text)) {
				warn("refusing session: %s", welcome.welcome.text);
				welcome.welcome.status = 1;
			}
			s->hello = 1;
			s->due = 0;
			welcome.welcome.src = s->self;
			if (ctrl_send(&s->ctrl, &welcome) || welcome.welcome.status) {
				session_close(s);
				return;
			}
			break;
		case CTRL_ERROR:
			warn("source: %s", msg.text);
			/* fall through */
		case CTRL_BYE:
			session_close(s);
			return;
		default:
			break;
		}
	}

	if (n < 0) {
		if (errno)
			warn("control connection: %m");
		session_close(s);
	}
}

static void session_accept(struct sink *sink)
{
	struct session *s = NULL;
	unsigned i;
	int fd;

	fd = accept4(sink->listen_fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return;

	for (i = 0; i < MAX_SESSIONS; i++) {
		if (!sink->sessions[i].used) {
			s = &sink->sessions[i];
			break;
		}
	}
	if (!s) {
		warn("too many sessions, refusing one");
		close(fd);
		return;
	}

	*s = (struct session){
		.used = 1,
		.due = ts_now(CLOCK_MONOTONIC) + HELLO_TIMEOUT_NS,
		.rev_fd = -1,
		.sched = { .fd = -1, .qos_fd = -1 },
		.batch = {
			[CTRL_KIND_RX] = {
				.type = CTRL_TS,
				.ts = { .flow = CTRL_FLOW_FWD, .kind = CTRL_KIND_RX },
			},
			[CTRL_KIND_TX] = {
				.type = CTRL_TS,
				.ts = { .flow = CTRL_FLOW_REV, .kind = CTRL_KIND_TX },
			},
		},
	};
	ctrl_conn_init(&s->ctrl, fd);
}

static void sink_probes(struct sink *sink)
{
	struct ptp_sync sync;
	struct session *s;
	struct ts_pair rx;

	while (ptp_recv_sync(sink->udp_fd, &sync, &rx) > 0) {
		s = session_find(sink, sync.domain, &sync.src);
		if (!s) {
			dbg("probe from unknown source, domain %u", sync.domain);
			continue;
		}

		s->fwd_seq = ptp_seq_extend(s->fwd_seq, sync.seq);
		batch_add(s, CTRL_KIND_RX, s->fwd_seq, ts_pick(&rx, s->mode),
			  sync.correction);
	}
}

static int sink_listen(struct sink *sink, const char *host)
{
	struct sockaddr_storage ss;
	socklen_t len;
	int zero = 0, one = 1;

	if (addr_resolve(host ? host : "::", sink->opts->port, &ss, &len))
		return -1;

	sink->listen_fd = socket(ss.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	sink->udp_fd = socket(ss.ss_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (sink->listen_fd < 0 || sink->udp_fd < 0)
		return sock_err("socket");

	if (ss.ss_family == AF_INET6) {
		setsockopt(sink->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
		setsockopt(sink->udp_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
	}
	setsockopt(sink->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	if (bind(sink->listen_fd, (struct sockaddr *)&ss, len) ||
	    listen(sink->listen_fd, 8))
		return sock_err("control port");
	if (bind(sink->udp_fd, (struct sockaddr *)&ss, len))
		return sock_err("probe port");
	/* Sessions of either mode share this socket. */
	if (ts_setup_sock(sink->udp_fd, TS_MODE_ANY))
		return sock_err("SO_TIMESTAMPING");

	return 0;
}

static void sink_loop(struct sink *sink)
{
	struct pollfd pfd[2 + 3 * MAX_SESSIONS];
	struct session *s;
	int64_t now, due;
	unsigned i, n;
	int timeout;

	while (!stop) {
		pfd[0] = (struct pollfd){ .fd = sink->listen_fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = sink->udp_fd, .events = POLLIN };
		n = 2;
		due = INT64_MAX;
		now = ts_now(CLOCK_MONOTONIC);

		for (i = 0; i < MAX_SESSIONS; i++) {
			s = &sink->sessions[i];
			if (!s->used)
				continue;

			if (s->due && now >= s->due) {
				if (s->hello) {
					batch_flush(s);
				} else {
					warn("no HELLO within %d s, closing",
					     (int)(HELLO_TIMEOUT_NS / NS_PER_S));
					session_close(s);
				}
				if (!s->used)
					continue;
			}
			if (s->due && s->due < due)
				due = s->due;

			pfd[n++] = (struct pollfd){ .fd = s->ctrl.fd, .events = POLLIN };
			pfd[n++] = (struct pollfd){ .fd = s->rev_fd, .events = POLLERR };
			pfd[n++] = (struct pollfd){ .fd = s->sched.fd, .events = POLLIN };
		}

		timeout = due == INT64_MAX ? -1 : (int)((due - now + NS_PER_MS - 1) / NS_PER_MS);
		if (poll(pfd, n, timeout) < 0)
			continue;

		if (pfd[1].revents)
			sink_probes(sink);

		/* pfd holds three entries per session that was in use
		 * when it was built; a session closing on the way only
		 * clears its own slot. */
		for (i = 0, n = 2; i < MAX_SESSIONS; i++) {
			s = &sink->sessions[i];
			if (!s->used)
				continue;
			if (pfd[n].revents)
				session_ctrl(sink, s);
			if (s->used && pfd[n + 1].revents)
				rev_txts(s);
			if (s->used && pfd[n + 2].revents)
				rev_send(s);
			n += 3;
		}

		if (pfd[0].revents)
			session_accept(sink);
	}
}

int cmd_sink(struct opts *opts)
{
	struct sink sink = { .opts = opts };
	struct sigaction sa = { .sa_handler = on_signal };
	unsigned i;

	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (sink_listen(&sink, opts->bind))
		return 1;

	sink_loop(&sink);

	for (i = 0; i < MAX_SESSIONS; i++)
		session_close(&sink.sessions[i]);
	close(sink.listen_fd);
	close(sink.udp_fd);
	return 0;
}

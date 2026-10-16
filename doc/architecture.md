ptping architecture
===================

README.md says what `ptping` does. This document says how it is built,
in enough detail to implement from.


Overview
--------

`ptping` is a single, single-threaded, Linux-only C program. It has no
dependencies beyond libc and the kernel headers. Its four commands are
two programs in disguise:

- `sink` runs the _sink_: a server that timestamps incoming probes and
  reports the timestamps back over a control connection. It serves
  any number of sources at once.

- `probe`, `stat` and `record` all run the _source_: the client that
  sends probes, collects timestamps from both ends, pairs them up into
  samples, and hands each sample to a command-specific consumer. The
  three commands differ only in what they do with the samples.

```
  source host                                   sink host
  ┌────────────────────────┐                    ┌────────────────────────┐
  │ source                 │   TCP 319 (ctrl)   │ sink                   │
  │  ctrl ◄────────────────┼────────────────────┼─► ctrl                 │
  │  tx ──► PTP Sync ──────┼── UDP 319 ─────────┼─► rx (+hw timestamp)   │
  │  rx ◄── PTP Sync ◄─────┼── UDP 319 ─────────┼── tx     [--bidir]     │
  │  errqueue (tx ts)      │                    │  errqueue (tx ts)      │
  └────────────────────────┘                    └────────────────────────┘
```

Every timestamp taken by the sink travels to the source over the
control connection. The source is the only place where samples are
assembled, and the only place that prints anything.


Terminology
-----------

- _Probe_: one PTP Sync message sent through the network under test.
- _Flow_: a direction. The _forward_ flow is source to sink. The
  _reverse_ flow, enabled by `--bidir`, is sink to source.
- _Sample_: the pair of (tx timestamp, rx timestamp) for one probe,
  plus its correctionField. A sample is _resolved_ when both
  timestamps have arrived, or when the loss timeout expires.
- _Session_: one source talking to one sink, i.e. one control
  connection and the flows that belong to it.


Probe packets
-------------

A probe is a PTP version 2 Sync message, carried over UDP on port 319
(the PTP event port), over IPv4 or IPv6. The header fields are:

| Field               | Value                                          |
|---------------------|------------------------------------------------|
| messageType         | 0 (Sync)                                       |
| majorSdoId          | 0                                              |
| versionPTP          | 2                                              |
| messageLength       | 44 plus any PAD TLV                            |
| domainNumber        | `--domain`, 254 by default                     |
| flagField           | twoStepFlag set, unicastFlag set               |
| correctionField     | 0 on transmission, read on reception           |
| sourcePortIdentity  | identifies the sender process, see below       |
| sequenceId          | 16-bit, increments by one per probe per flow   |
| controlField        | 0                                              |
| logMessageInterval  | 0x7f                                           |
| originTimestamp     | 0                                              |

The twoStepFlag is set because `ptping` never relies on an on-wire
timestamp; it always collects the transmission timestamp from the
socket error queue. One-step hardware is free to overwrite
originTimestamp, and the sink ignores it.

### sourcePortIdentity

The clockIdentity is the EUI-64 derived from the MAC address of the
interface the probes leave through, exactly as `ptp4l` does. The
portNumber is `(getpid() % 65535) + 1`, so that several `ptping`
processes on one host are distinguishable on the sink. The pair is
sent to the sink in the HELLO message, and the sink uses it to map
received probes to sessions.

### Padding

`--size` is the Ethernet frame size on the wire, including the
Ethernet header and the FCS. The probe is padded to that size with a
single PAD TLV (tlvType 0x8008, IEEE 1588-2019) appended to the Sync
message; messageLength covers the TLV. Using a real TLV rather than
trailing garbage keeps the message valid to hardware that parses it
before timestamping. The smallest frame is 90 bytes over IPv4 and 110
over IPv6, a bare Sync. The next possible size is four bytes larger,
since the TLV header is four bytes, and from there every even size
works. Anything else is an error.

### Sequence numbers

sequenceId is 16 bits and wraps every 65536 probes, under eleven
minutes at the default rate. Both ends extend it to 64 bits per flow:

    ext = last_ext + (int16_t)(seq - (uint16_t)last_ext)

This tolerates loss and reordering within a window of 32767 probes,
which is far more than the loss timeout allows to be in flight. No
private TLV is needed, and the packet stays a plain Sync.

### DSCP

DSCP is set with `IP_TOS` / `IPV6_TCLASS` on the UDP socket. Probes
only ever leave through regular UDP sockets; there is no raw socket
path. Users who need tagged probes route them through a VLAN
interface and let its egress priority map set the PCP.


Timestamping
------------

All timestamping goes through `SO_TIMESTAMPING`; see `ts.c`. The
following applies to any socket that sends or receives probes, on both
hosts:

- A socket asks for one kind of timestamp only, hardware or
  software, for both reception and transmission. Requesting both
  would fill two slots of every report and queue two transmission
  reports per packet, for no benefit once the mode is decided.

- Reception timestamps arrive in a `SCM_TIMESTAMPING` control
  message; index 2 is the raw hardware timestamp, index 0 the
  software one, and only one of them is ever filled.

- Transmission timestamps are reported on `MSG_ERRQUEUE`. `OPT_ID`
  and `OPT_TSONLY` make the kernel report a per-socket send counter
  instead of a copy of the packet. Probe sockets send nothing but
  probes, so that counter is the sequence number. This is simpler
  than parsing the looped-back packet, whose headers depend on the
  link type, and does not depend on `net.core.tstamp_allow_data`.

Hardware timestamping is a per-interface setting, `SIOCSHWTSTAMP`. On
startup, and on the sink whenever a new session arrives on a
previously unseen interface, `ptping`:

1. Reads the current configuration with `SIOCGHWTSTAMP`.
2. Leaves it alone if `tx_type` is not `OFF` and `rx_filter` already
   covers L4 Sync messages (`ALL`, `PTP_V2_EVENT`, `PTP_V2_L4_EVENT`,
   `PTP_V2_SYNC` or `PTP_V2_L4_SYNC`). This is the common case when
   `ptp4l` is already running, and it must not be disturbed.
3. Otherwise sets `HWTSTAMP_TX_ON` with `HWTSTAMP_FILTER_PTP_V2_EVENT`,
   falling back to `PTP_V2_L4_EVENT` and then `ALL`, in the same way
   `linuxptp` does. Widening an L2-only filter to L2+L4 is harmless
   to a running `ptp4l`.

On the source, if this fails and `--timestamping` is `hw`, the
command fails with a message naming the interface. With `any`, it
falls back to software timestamps and says so. With `sw`, the ioctl
is never attempted. On the sink the mode comes from HELLO, so a
hardware session that cannot be served is refused instead.

The interface is found the same way on both ends: `connect()` an
unbound UDP socket to the peer, the sink address on the source and
the control connection's peer address on the sink, read the local
address back with `getsockname()`, and match it against
`getifaddrs()`. A link-local address carries the interface in its
scope id directly. The control connection and the probes share a
path, so this is the interface the probes cross.

### Clocks

Hardware timestamps are in the PHC's timescale, software timestamps
in `CLOCK_REALTIME`. The source resolves its mode on its own
interface and names it in HELLO; the sink delivers that mode on the
interface the session arrived on, or refuses. The sink has no mode of
its own, which is why `--timestamping` is a source option. Latency is
simply `rx - tx`, with no conversion. `ptping` never reads the timestamping clocks
itself; the loss timeout and the plain-rate schedule use
`CLOCK_MONOTONIC`, and a windowed schedule `CLOCK_REALTIME` with the
TAI offset, see "Transmission schedule".

Timestamps are `int64_t` nanoseconds everywhere, in memory and on the
control connection. The correctionField is kept as the raw signed
64-bit value in units of 2^-16 ns and only converted for display.

### Port 319 and ptp4l

Both ends bind UDP port 319, because the hardware filters that
identify PTP on reception key on the destination port. This conflicts
with a `ptp4l` instance using UDP transport on the same host. The
sink refuses to start in that case. The source falls back to an
ephemeral port with a warning, unless `--bidir` is in effect, in which
case it also refuses. The README advises running `ptp4l` with its L2
transport, which uses raw sockets and leaves 319 free.


Control protocol
----------------

The source connects to the sink on TCP port 319 (registered to PTP,
otherwise unused). The stream is a sequence of messages, each with a
four-byte header. All integers are big-endian.

    struct ctrl_hdr {
        uint8_t  type;
        uint8_t  flags;     /* reserved, 0 */
        uint16_t len;       /* payload bytes */
    };

| Type | Name    | Direction      | Purpose                                 |
|------|---------|----------------|-----------------------------------------|
| 1    | HELLO   | source to sink | open a session, describe the flows      |
| 2    | WELCOME | sink to source | accept or refuse                        |
| 3    | TS      | sink to source | batch of timestamps                     |
| 4    | BYE     | either         | orderly close                           |
| 5    | ERROR   | either         | fatal problem, text follows, then close |

HELLO:

    uint8_t  version;        /* 1 */
    uint8_t  flags;          /* bit 0: bidir */
    uint8_t  domain;
    uint8_t  tstamp;         /* 0 hw, 1 sw: the session's mode */
    uint8_t  clock_id[8];    /* source's sourcePortIdentity */
    uint16_t port_number;
    /* reverse flow parameters, ignored unless bidir */
    uint64_t interval_ns;
    uint32_t count;          /* 0: until BYE */
    uint16_t size;
    uint8_t  dscp;
    uint8_t  observe;        /* send throughout the cycle, only tag */
    int64_t  cycle_ns;       /* 0: no window */
    int64_t  base_time;      /* CLOCK_TAI */
    int64_t  window_offset_ns;
    int64_t  window_len_ns;
    int64_t  txtime_lead_ns; /* 0: no SO_TXTIME */

WELCOME:

    uint8_t  version;
    uint8_t  status;         /* 0 ok, otherwise refused, text follows */
    uint8_t  reserved[2];
    uint8_t  clock_id[8];    /* sink's sourcePortIdentity, for reverse */
    uint16_t port_number;
    char     text[];         /* human-readable reason when refused */

TS carries one or more records of the same flow and kind:

    uint8_t  flow;           /* 0 forward, 1 reverse */
    uint8_t  kind;           /* 0 rx, 1 tx */
    uint16_t count;
    struct {
        uint64_t seq;        /* extended sequence number */
        int64_t  ts;         /* ns */
        int64_t  correction; /* correctionField, 2^-16 ns; 0 for tx */
    } rec[count];

The sink sends forward-rx records for probes it receives, and, with
bidir, reverse-tx records for probes it sends. The source already has
forward-tx and reverse-rx timestamps locally. Records are batched: the
sink flushes a batch when it reaches 64 records or 50 ms after the
first record was added, whichever comes first. The source never sends
TS.

Session lifecycle:

1. Source connects, sends HELLO.
2. Sink looks up the interface, configures timestamping, registers
   the sourcePortIdentity, starts the reverse flow if asked, replies
   WELCOME. On any failure it replies WELCOME with a non-zero status
   and closes.
3. Source starts sending.
4. Source stops sending when the count is reached or on SIGINT, waits
   one loss timeout for stragglers, sends BYE, prints its summary.
5. Sink stops the reverse flow and forgets the session when it reads
   BYE or when the connection drops.

TCP keepalive is enabled with a short interval so a dead peer is
noticed within a few seconds.


Source
------

The source is an event loop around `poll()` with these descriptors:

- the control connection,
- the probe socket (readable for reverse probes and, with `POLLERR`,
  for transmission timestamps on the error queue),
- a `timerfd` driving transmission,
- a `timerfd` for the periodic report.

SIGINT and SIGTERM set a counter that the loop checks on every
wake-up: the first stops sending and starts the drain, the second
ends the run at once. The loss timeout sweep also runs on every
wake-up, since it only ever looks at the oldest sample of each flow.

### Flows

Per flow, `flow.c` keeps a table of unresolved samples keyed by
extended sequence number, with the `CLOCK_MONOTONIC` time the sample
was created. A sample is created when the probe is sent (forward) or
when the first of its timestamps arrives (reverse). When the second
timestamp arrives the sample is resolved and handed to the consumer.
The sweep marks samples older than the loss timeout (`-W`, default
1 s) as lost, resolving them with the missing fields unset. Once
sending is over, the source drains for one more loss timeout, or
until nothing is outstanding when the count was finite.

The consumer is a callback that gets samples in sequence order. The
flow holds back a resolved sample until all lower sequence numbers
are resolved too, which costs at most one loss timeout of delay after
a loss and gives `record` a monotonically ordered output for free.

A sample has four states for its timestamps: both present, tx missing
(the NIC never reported it; some NICs can only track one outstanding
tx timestamp), rx missing (lost in the network or on the sink), and
both missing (reverse flow, nothing ever arrived). `stat` counts them
separately, since "no tx timestamp" is a local problem rather than a
network one.

### Transmission schedule

`sched.c` answers one question: when is the next probe due? Due
times step by exact integer periods from the start time rather than
from the previous transmission, so the rate does not drift.

A schedule limits them to a window: a cycle time, a base time and one
window given as offset and length within the cycle, which is how one
gate of an 802.1Qbv gate control list looks from the outside. A due
time outside the window moves to the start of the next window, so
each window carries probes at its start plus multiples of the period
while it is open, and a window shorter than the period gives exactly
one probe per cycle at the window's offset.

The schedule is in `CLOCK_TAI`, which is the PHC's timescale under
ptp4l and phc2sys, and the clock tc's taprio uses for its base time.
`timerfd` has no TAI clock, so a scheduled flow runs its timer on
`CLOCK_REALTIME` with the TAI offset, measured once at start, applied
to every due time. A plain rate stays on `CLOCK_MONOTONIC`. The
schedule owns its timer, since it alone knows which clock it needs.

Timer-driven sending places a probe within tens of microseconds of
its due time. For better, a probe can carry an `SO_TXTIME` launch
time for the `etf` qdisc, in which case the timer fires a lead time
early and the qdisc, or the NIC with offload, releases the frame at
the due time. This is opt-in, since without `etf` the kernel would
send as soon as it got the frame.

A window can also be observed instead of enforced: the sender runs
at its rate throughout the cycle, on the plain clock, and the window
only serves to tag each sample by what the timestamps show, whether
it left inside the window and whether it arrived inside it. The four
tags, conformant, early, late and non-conformant, are observations,
not verdicts; interpretation is left to the reader. `record` writes
both phases, the number of cycle boundaries between them and the
tag; `stat` keeps a statistics block per tag. A hardware timestamp
is in the PHC's timescale already; a software one is in
`CLOCK_REALTIME` and gets the TAI offset added. Observing needs no
placement precision from the source, so it works where enforcing
does not.

The reverse flow follows the same schedule, which HELLO carries.

### Consumers

- `probe` sends one probe, with `--count 1` forced, waits for it to
  resolve, and prints a verdict: timestamp sources at each end,
  latency, correctionField (nonzero means transparent clocks in the
  path, and the residence time is printed), and a warning when the
  latency exceeds 1 ms, which on a switched network means something
  forwarded the probe in software. Exit status is non-zero when the
  probe was lost or either timestamp is missing.

- `stat` feeds samples into `stats.c` and prints a report every
  interval (`-i`, default 1 s). On a terminal the report overwrites
  itself in place; otherwise one report is printed per interval. The
  final report on exit covers the whole run.

- `record` writes one CSV line per sample, in sequence order within
  each flow, with the header
  `flow,seq,tx_ns,rx_ns,correction,latency_ns,tx_phase_ns,rx_phase_ns,cycles,tag`.
  Missing fields are empty, and the last four are only there with a
  schedule. Lines are flushed every report interval.

### Statistics

`stats.c` keeps, per flow: count sent, received, lost, missing tx
timestamp; min, max, running mean and variance (Welford) for mdev;
and a log-linear histogram for percentiles. The histogram has one
bucket per power of two of nanoseconds, split into 128 linear
sub-buckets, giving better than 1 % resolution from 1 ns up to days,
in a fixed 58 kB of memory. Percentiles are read by walking the
buckets. This is the usual HdrHistogram layout, cut down to what is
needed.


Sink
----

The sink is the same kind of `poll()` loop with:

- the listening TCP socket,
- one UDP socket on port 319 (dual-stack IPv6 unless the bind address
  is IPv4), which only ever receives,
- one control connection per session,
- per session with a reverse flow, a transmission `timerfd` and a
  UDP socket of its own, bound to an ephemeral port and connected to
  the source, which carries the reverse probes and reports their
  transmission timestamps. A socket per flow is what makes the
  kernel's send counter equal to the sequence number. Reverse probes
  therefore come from an ephemeral port, so the source must not
  connect its probe socket.

Batch flushing and the HELLO timeout need no timer of their own; the
poll timeout is computed from the nearest deadline.

Sessions live in a table keyed by domain and sourcePortIdentity. A
received probe is parsed with `ptp.c`, dropped unless it maps to a
session, extended to a 64-bit sequence number, and appended to the
session's forward-rx batch along with the reception timestamp and
correctionField. Reverse probes carry the sink's own
sourcePortIdentity and go to the control connection's peer address on
the probe port; their tx timestamps go to the reverse-tx batch.

The sink has no notion of samples or loss. It reports what it sees
and leaves the pairing to the source, which keeps the sink stateless
across source restarts and keeps the protocol one-directional for
data.

The sink runs without a bind address by default and configures
hardware timestamping per interface when a session arrives on it. An
interface already configured is left alone, so repeating this per
session costs one ioctl.

The shared receive socket asks for both hardware and software
timestamps, and the sink picks the one a probe's session uses. A
hardware session and a software session can therefore share an
interface, each with timestamps from the clock the source expects.


Code layout
-----------

    src/
      ptping.c    main(): common options, command dispatch
      sink.c      sink command
      source.c    source event loop, session setup, shared by the three
                  source commands
      probe.c     probe consumer and verdict
      stat.c      stat consumer and report formatting
      record.c    record consumer, CSV output
      ctrl.c/h    control protocol: framing, encode/decode of each
                  message, buffered reads on a non-blocking socket
      ptp.c/h     Sync message build and parse, PAD TLV, sequence
                  number extension, EUI-64 clockIdentity
      ts.c/h      SO_TIMESTAMPING setup, SIOCGHWTSTAMP/SIOCSHWTSTAMP
                  negotiation, cmsg and error queue parsing
      netif.c/h   interface lookup from a local address, MAC address
      sched.c/h   next transmission time: a rate, limited to a window
                  of a cyclic schedule; owns the timerfd
      flow.c/h    per-flow sample table, loss timeout, in-order
                  delivery to the consumer
      stats.c/h   counters, min/max/mean/mdev, histogram, percentiles
      log.h       err/warn/info/debug to stderr, -v controls level
      tspec.h     int64 ns helpers, timespec conversion
    test/
      unit tests for ptp.c, ctrl.c, flow.c, stats.c, netif.c, ts.c
      and sched.c, run by `make check`
      e2e.sh: two network namespaces joined by a veth pair, software
      timestamping, runs probe, stat and record end to end

Dependencies between modules point downwards only: the commands use
`source.c`/`sink.c`, which use `ctrl`, `ptp`, `ts`, `netif`,
`sched`, `flow` and `stats`, which use only `log` and `tspec`.


Privileges
----------

| Capability           | Needed for                                     |
|----------------------|------------------------------------------------|
| CAP_NET_BIND_SERVICE | binding UDP and TCP port 319                   |
| CAP_NET_ADMIN        | SIOCSHWTSTAMP, when the interface is not       |
|                      | already configured                             |

Running as root is the expected case. Nothing is dropped after
startup, since the sink may need SIOCSHWTSTAMP for any later session.


Build and test
--------------

Autotools, C11, `-Wall -Wextra -Werror`. `configure` checks for the
Linux headers that define `SO_TIMESTAMPING` and
`SOF_TIMESTAMPING_OPT_CMSG`, and fails on anything else. `make check`
runs the unit tests and `test/e2e.sh`.

The end-to-end test uses `--timestamping sw`. As root it joins two
network namespaces with a veth pair; otherwise it runs both ends on
loopback, with distinct addresses so both can own the probe port, on
an unprivileged port. Either way everything except the hardware
timestamping path is exercised on any machine. The hardware path is
tested by hand on real NICs.


Open questions
--------------

- Whether the slow-path warning threshold in `probe` should scale
  with something the user supplies, such as a hop count.
- Whether to let the sink serve more than one domain at once.

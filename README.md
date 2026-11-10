Precisely Timed Ping
====================

`ptping` measures the forwarding latency of a network. In contrast to
`ping(8)`, it offers high precision measurements by:

- Using L4 PTP Sync messages as probe packets. This allows it to reuse
  the packet timestamping functionalities available in many commodity
  NICs and switches.

- Scheduling transmissions according to a cyclical schedule, suitable
  for testing the performance of IEEE 802.1Qbv networks.

Similar to `iperf3(1)`, a `ptping` _source_ communicates with another
`ptping` _sink_. Parallel to the probe packets, a TCP control
connection is used to communicate results back to the source.


Clock synchronization
---------------------

A one-way latency is the difference between a reception timestamp
taken at the sink and a transmission timestamp taken at the source.
These are taken by two different clocks, so any offset between them
ends up directly in every sample, and any wander between them ends up
in the measured jitter. `ptping` does not synchronize the clocks, it
only relies on them being synchronized:

- With hardware timestamping, the PHCs of the source and sink NICs
  must be synchronized, typically by running `ptp4l(8)` on both hosts
  in a PTP domain _other_ than the one `ptping` uses.

- With software timestamping, the system clocks must be synchronized,
  e.g. by `phc2sys(8)` tracking a synchronized PHC.

- With a schedule (see the schedule options), the probes are released
  by the system clock, so on the host that sends them it must track
  the PHC, again with `phc2sys(8)`. That is the source, and with
  `--bidir` the sink too. The UTC offset must be set as well, which
  `phc2sys -w` does from what `ptp4l` learned, so that `CLOCK_TAI` is
  the PHC's timescale and not 37 seconds behind it; `ptping` warns
  when it finds no offset.

As a sanity check of the synchronization, the source can ask the sink
to send probes in the reverse direction as well (`--bidir`). The sum
of the two one-way latencies is independent of the clock offset, and
the difference between them reveals the offset and/or path asymmetry.


Caveats
-------

Using PTP messages as probes means that the network under test may
treat them as PTP, rather than as regular traffic:

- Many switches trap all PTP event messages (UDP port 319) to their
  CPU, regardless of PTP domain. If so, the probes take the slow path
  through the switch, and the measured latency says nothing about the
  data path. Latencies that are orders of magnitude larger than
  expected are a sure sign of this. `probe` warns when it sees them.

- Transparent clocks add their residence time to the correctionField
  of the Sync message, typically in all domains. `ptping` records the
  correctionField of every received probe, so the residence time can
  be separated from the propagation delay.

- Boundary clocks that are not members of `ptping`'s domain will
  either forward the probes as regular traffic or drop them. `probe`
  reports which.

On the hosts themselves, `ptping` binds UDP port 319, which a `ptp4l`
instance using UDP transport will already hold. Run `ptp4l` with its
L2 transport (`-2`) on hosts that also run `ptping`.


HOWTO
-----

### Create a sink

First, launch a _sink_ on the host attached to the egress point of the
network under test. In simple setups, `ptping` can typically be run
without any additional arguments:

    user@end:~# ptping sink

In case multiple addresses are assigned to the host running the sink,
you can specify which one you want to bind to. Link-local addresses
must carry a scope:

    user@end:~# ptping sink fe80::dead:1%eth0

### Test connection

Before running any measurements, ensure that the sink is reachable from
the source, that timestamps can be collected both at the source and at
the sink, and that the probes are forwarded as regular traffic:

    user@start:~# ptping probe fe80::dead:1%eth0

### Testing a gate

With a schedule, probes are only sent while a chosen window of the
cycle is open. To send one probe per 1 ms cycle, 200 us after the
cycle starts, through a gate that is open from 150 us to 350 us:

    user@start:~# ptping stat -r 1000 --cycle 1ms --window 200us+1us fe80::dead:1%eth0

Shift the window across the cycle to find where the gate opens and
closes, and compare the latency inside and outside of it.

### Source

Once the connection is known to be good, you are ready to launch a real
test. The `stat` command outputs a continuously updating summary of the
current session:

    user@start:~# ptping stat fe80::dead:1%eth0
    source -> sink: 2043 sent (101 pps), 2040 received (100 pps), 3 lost (0.1%)

    latency (us)     count       min       avg       p99     p99.9       max
    all               2040     2.350     2.390     2.421     2.460     2.468

With a window, the table gets a row each for the probes that left
inside and outside of it, and with `--bidir` a second block of rows
for the reverse flow.

To save every sample for later analysis, use `record` instead:

    user@start:~# ptping record -o run1.csv fe80::dead:1%eth0

### Comparing streams

One source can run several streams at once, one per sink address on
the command line. Each address gets the options given before it, so
to compare how three classes cross the same gate, change only the
DSCP between them:

    user@start:~# ptping stat --cycle 1ms --window 200us+100us \
                    -n voice -d cs7 fe80::dead:1%eth0 \
                    -n video -d cs5 fe80::dead:1%eth0 \
                    -n bulk  -d 0   fe80::dead:1%eth0

To compare two paths through the network, name a sink address on
each:

    user@start:~# ptping stat 10.0.1.2 10.0.2.2

`stat` gives every stream its own lines, and `record` names the
stream in the first column.

### Viewing a recording

`contrib/ptping-view` plots recordings in a browser:

    user@start:~# contrib/ptping-view run1.csv
    user@start:~# contrib/ptping-view recordings/

It offers every CSV in the directory, opening on the given file if
there is one. The views are a summary table like `stat`'s, the
latency CCDF of each stream, latency and arrival time against the
position in the cycle, latency over time, and a raw scatter plot of
any two columns. Reloading the page reads the file again, so a
recording still being written can be followed. The script needs only
Python 3; the page loads Plotly from a CDN.


Usage
-----

```
ptping [<common-options>] <command> [<command-options-and-arguments>]

ptping sink   [<BIND-ADDRESS>]
ptping probe  <STREAM>...
ptping stat   <STREAM>...
ptping record [-o <OUTPUT-FILE>] <STREAM>...
ptping <STREAM>...

<STREAM> is [<source-options>] <SINK-ADDRESS>

Common options:
  -p, --port N
      Use port N for probes and for the control connection. Hardware
      only recognizes PTP on port 319, so this is for testing with
      software timestamps, where it allows running without root.

      By default, port 319 is used.

  -i, --interval <time>
      Print a `stat` report every <time>.

      By default, one is printed every second.

  -o, --output <OUTPUT-FILE>
      Make `record` write to <OUTPUT-FILE>.

      By default, it writes to stdout.

Commands:
  sink [<BIND-ADDRESS>]
      Listen for connections from sources on <BIND-ADDRESS>, if
      specified, otherwise on any address. Record timestamps of
      incoming probe packets and relay the results back to the source.

      A sink serves multiple sources concurrently. Probes are matched
      to sources by their sourcePortIdentity and sequenceId.

  probe <STREAM>...
      Verify that the sink accepts connections. Send a single probe
      packet, make sure that the transmission timestamp can be
      retrieved, and that the sink relays back the reception
      timestamp. Warn if the latency suggests that the probe was
      forwarded in software somewhere along the path.

  stat <STREAM>...
      Continuously send probe packets to the sink and track key
      statistics about packets transmitted, received and lost,
      min, average, p99, p99.9 and max latencies, etc.

  record [-o <OUTPUT-FILE>] <STREAM>...
      Record latency measurements and write the results to
      <OUTPUT-FILE> if specified, otherwise to stdout. Output is CSV
      with one line per probe, containing the stream, direction,
      sequence number, transmission timestamp, reception timestamp,
      correctionField and latency. Lost probes are detected from gaps
      in the sequence numbers and recorded with empty reception
      fields.

  <STREAM>...
      Shorthand for `stat <STREAM>...`.

Streams:
  Each <SINK-ADDRESS> is a stream of probes to that sink, sent with
  the source options given before it, on the command line or before
  the command. An option stays in effect for every later address
  until it is given again, so streams that differ in one parameter
  only need that option repeated. --name is the exception: it only
  applies to the next address.

  The streams run at the same time, each with a session of its own,
  so they may go to different sinks or to the same one. When a
  stream's sink goes away, that stream ends and the rest carry on.

Source options:
  -n, --name <name>
      Name the stream in the output of every command.

      By default, a stream is named after its <SINK-ADDRESS>, with
      #2, #3 and so on added when an earlier stream has that name.
      `record` refuses a name with a comma in it, which would split
      its CSV field.

  -D, --domain N
      Operate in PTP domain N. The user must ensure that the domain is
      not used for time synchronization.

      By default, operate in domain 254.

  -T, --timestamping hw|sw|any
      hw:  Require hardware timestamping support.
      sw:  Use software timestamping.
      any: Use hardware timestamping if it is available, otherwise fall
           back to software timestamping.

      The source decides, for both ends. The sink delivers the mode
      asked for on the interface the session arrives on, or refuses
      the session. Note that "any" only looks at the source's own
      interface; if the sink cannot do hardware timestamping, rerun
      with "sw".

      By default, hardware timestamping support is required.

  -b, --bidir
  --no-bidir
      Also measure the latency from the sink to the source, or stop
      doing so. Reported as a separate flow in `stat` and `record`.
      See "Clock synchronization" above for why this is useful.

  -c, --count <n>
      Exit after sending <n> packets.

      By default, transmission continues until it is interrupted by the
      user.

  -d, --dscp <val>|cs<n>
      Use a DSCP of <val> on probe packets. Many switches can map the
      DSCP to a traffic class, which makes this the easiest way to
      select the egress queue under test.

      By default, CS5 is used.

  -r, --rate <rate>|<time>
      Send <rate> packets per second, or one packet every <time>. A
      rate takes decimals and an optional hz: 100, 4.7hz. A time takes
      a unit, as below: 10ms, 1.337ms. At most one packet per
      microsecond.

      By default, 100 pps are sent.

  -s, --size <size>
      Use probe frames that are <size> bytes long on the wire,
      including the Ethernet header and FCS. Frames are padded using
      the PAD TLV defined by IEEE 1588-2019, so that they remain valid
      PTP messages to any hardware that inspects them before
      timestamping.

      By default, the smallest frame that fits a Sync message is used:
      90 bytes over IPv4, 110 bytes over IPv6.

  -W, --timeout <time>
      Consider a probe lost if its reception timestamp has not arrived
      within <time> of sending it.

      By default, 1 second.

  Every <time> takes an optional unit, ns, us, ms or s, and decimals,
  as in 1.5ms. Without a unit, the schedule options below are in
  nanoseconds, and --interval and --timeout are in seconds.

Schedule options:

  These limit transmissions to a window of a cyclic schedule, such as
  one gate of an IEEE 802.1Qbv gate control list. Probes keep their
  rate, but one that would fall outside the window waits for the next
  window to open. A window shorter than the time between probes thus
  gives one probe per cycle, at the window's offset.

  The schedule is kept in CLOCK_TAI,
  which is the PHC's timescale when ptp4l and phc2sys are running,
  and the same timescale that tc's taprio uses; see "Clock
  synchronization" above for what the sending host must run. The
  reverse flow of --bidir follows the same schedule.

  --cycle <time>
      Cycle time of the schedule.

  --window <start>-<end>
  --window <start>+<length>
      Send only between <start> and <end> into each cycle, or for
      <length> from <start>. 5us-245us and 5us+240us are the same
      window.

  --base-time <time>
      Cycles start at this time, in CLOCK_TAI, as for tc's taprio.

      By default, 0: cycles are aligned to multiples of the cycle time
      since the epoch.

  --observe
  --no-observe
      Keep sending throughout the cycle as if there were no window,
      and instead tag every probe by whether it left the source inside
      the window or outside it. `stat` reports the two separately,
      `record` writes the tag per sample.

      --no-observe goes back to sending only within the window.

      This shows what the network does to the class at every phase of
      the cycle, and does not depend on the source placing its
      transmissions precisely. Pick a rate whose period does not
      divide the cycle, so that the probes walk through every phase
      rather than hitting the same one each cycle; a gap of 1.003ms
      walks through a 1 ms cycle in 333 probes.

  --txtime <time>
      Hand each probe to the kernel this much before it is due, with
      its launch time set, for an etf qdisc to send it on time.
      Without etf on the interface the kernel would send early, which
      is why this is not on by default. Needs CAP_NET_ADMIN.

      Without this, probes are sent from a timer, which places them
      within tens of microseconds of their due time, provided ptping
      runs as root: it then asks the kernel to keep the CPUs out of
      deep idle states, from which waking up alone takes over 100 us.
```

The timestamps of each probe tell where in the cycle it actually left
and arrived. Each probe is tagged "inside" or "outside" by where it
left the source relative to the window, and `stat` groups its
latencies by that tag. The tag says where the probe left, not what
the network did to it; `record` also writes both phases and how many
cycle boundaries lie between them, which is what a plot or a later
analysis needs to tell a probe held over to the next window from one
that got through in the same cycle.


Building
--------

`ptping` uses a standard autotools setup, so building essentially boils
down to:

    ./configure && make && make install

If you are building from a GIT tree and not a release tarball, you must
first generate the `configure` script:

    ./autogen.sh

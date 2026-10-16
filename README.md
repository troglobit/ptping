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

    user@start:~# ptping stat -r 1000 --cycle 1000000 --window 200000:1000 fe80::dead:1%eth0

Shift the window across the cycle to find where the gate opens and
closes, and compare the latency inside and outside of it.

### Source

Once the connection is known to be good, you are ready to launch a real
test. The `stat` command outputs a continuously updating summary of the
current session:

    user@start:~# ptping stat fe80::dead:1%eth0
    Packets: 2043/101pps -> 2040/100pps source -> sink, 3 lost (0.1%)
    Latency: 2.350/2.390/2.468/0.054 us min/avg/max/mdev
    Tail:    2.421/2.460 us p99/p99.9

To save every sample for later analysis, use `record` instead:

    user@start:~# ptping record fe80::dead:1%eth0 run1.csv


Usage
-----

```
ptping [<common-options>] <command> [<command-options-and-arguments>]

ptping sink   [<sink-options>] [<BIND-ADDRESS>]
ptping probe  [<source-options>] <SINK-ADDRESS>
ptping stat   [<source-options>] <SINK-ADDRESS>
ptping record [<source-options>] <SINK-ADDRESS> [<OUTPUT-FILE>]
ptping [<source-options>] <SINK-ADDRESS>

Common options:
  -D, --domain N
      Operate in PTP domain N. The user must ensure that the domain is
      not used for time synchronization.

      By default, operate in domain 254.

  -p, --port N
      Use port N for probes and for the control connection. Hardware
      only recognizes PTP on port 319, so this is for testing with
      software timestamps, where it allows running without root.

      By default, port 319 is used.

Commands:
  sink [<sink-options>] [<BIND-ADDRESS>]
      Listen for connections from sources on <BIND-ADDRESS>, if
      specified, otherwise on any address. Record timestamps of
      incoming probe packets and relay the results back to the source.

      A sink serves multiple sources concurrently. Probes are matched
      to sources by their sourcePortIdentity and sequenceId.

  probe [<source-options>] <SINK-ADDRESS>
      Verify that the sink accepts connections. Send a single probe
      packet, make sure that the transmission timestamp can be
      retrieved, and that the sink relays back the reception
      timestamp. Warn if the latency suggests that the probe was
      forwarded in software somewhere along the path.

  stat [<source-options>] <SINK-ADDRESS>
      Continuously send probe packets to the sink and track key
      statistics about packets transmitted, received and lost,
      min/avg/max/mdev and p99/p99.9 latencies, etc.

  record [<source-options>] <SINK-ADDRESS> [<OUTPUT-FILE>]
      Record latency measurements and write the results to
      <OUTPUT-FILE> if specified, otherwise to stdout. Output is CSV
      with one line per probe, containing the direction, sequence
      number, transmission timestamp, reception timestamp,
      correctionField and latency. Lost probes are detected from gaps
      in the sequence numbers and recorded with empty reception
      fields.

  <SINK-ADDRESS>
      Shorthand for `stat <SINK-ADDRESS>`.

Source options:

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
      Also measure the latency from the sink to the source. Reported
      as a separate flow in `stat` and `record`. See "Clock
      synchronization" above for why this is useful.

  -c, --count <n>
      Exit after sending <n> packets.

      By default, transmission continues until it is interrupted by the
      user.

  -d, --dscp <val>|cs<n>
      Use a DSCP of <val> on probe packets. Many switches can map the
      DSCP to a traffic class, which makes this the easiest way to
      select the egress queue under test.

      By default, CS5 is used.

  -i, --interval <seconds>
      Print a `stat` report every <seconds>.

      By default, one is printed every second.

  -r, --rate <rate>
      Send <rate> packets per second.

      By default, 100 pps are sent.

  -s, --size <size>
      Use probe frames that are <size> bytes long on the wire,
      including the Ethernet header and FCS. Frames are padded using
      the PAD TLV defined by IEEE 1588-2019, so that they remain valid
      PTP messages to any hardware that inspects them before
      timestamping.

      By default, the smallest frame that fits a Sync message is used:
      90 bytes over IPv4, 110 bytes over IPv6.

  -W, --timeout <seconds>
      Consider a probe lost if its reception timestamp has not arrived
      within <seconds> of sending it.

      By default, 1 second.

Schedule options:

  These limit transmissions to a window of a cyclic schedule, such as
  one gate of an IEEE 802.1Qbv gate control list. Probes keep their
  rate, but one that would fall outside the window waits for the next
  window to open. A window shorter than the time between probes thus
  gives one probe per cycle, at the window's offset.

  All times are in nanoseconds. The schedule is kept in CLOCK_TAI,
  which is the PHC's timescale when ptp4l and phc2sys are running,
  and the same timescale that tc's taprio uses; see "Clock
  synchronization" above for what the sending host must run. The
  reverse flow of --bidir follows the same schedule.

  --cycle <ns>
      Cycle time of the schedule.

  --window <offset>:<length>
      Send only between <offset> and <offset> + <length> into each
      cycle.

  --base-time <ns>
      Cycles start at this time, in CLOCK_TAI, as for tc's taprio.

      By default, 0: cycles are aligned to multiples of the cycle time
      since the epoch.

  --observe
      Keep sending throughout the cycle as if there were no window,
      and instead tag every probe by what was observed of it: whether
      it left the source inside the window, and whether it arrived at
      the sink inside it. `stat` reports each tag separately, `record`
      writes it per sample.

      This shows what the network does to the class at every phase of
      the cycle, and does not depend on the source placing its
      transmissions precisely. Pick a rate whose period does not
      divide the cycle, so that the probes walk through every phase
      rather than hitting the same one each cycle; 1 ms plus 7 us
      covers a 1 ms cycle in 143 probes.

  --txtime <lead>
      Hand each probe to the kernel <lead> nanoseconds before it is
      due, with its launch time set, for an etf qdisc to send it on
      time. Without etf on the interface the kernel would send early,
      which is why this is not on by default. Needs CAP_NET_ADMIN.

      Without this, probes are sent from a timer, which places them
      within tens of microseconds of their due time, provided ptping
      runs as root: it then asks the kernel to keep the CPUs out of
      deep idle states, from which waking up alone takes over 100 us.
```

The timestamps of each probe tell where in the cycle it actually left
and arrived. From them, each probe gets one of four tags:

| Tag            | Left the source | Arrived at the sink |
|----------------|-----------------|---------------------|
| conformant     | inside          | inside              |
| early          | outside         | inside              |
| late           | inside          | outside             |
| non-conformant | outside         | outside             |

The tags say what happened, not why; with a gate in the path, "early"
probes are the ones it held until the window opened, and
"non-conformant" ones passed while it should have been closed.
`record` also writes both phases and how many cycle boundaries lie
between them, so that a probe held over to the next window can be
told from one that got through in the same cycle.


Building
--------

`ptping` uses a standard autotools setup, so building essentially boils
down to:

    ./configure && make && make install

If you are building from a GIT tree and not a release tarball, you must
first generate the `configure` script:

    ./autogen.sh

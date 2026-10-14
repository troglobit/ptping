Time-aware Ping
===============

`taping` measures the forwarding latency of a network. In contrast to
`ping(8)`, it offers high precision measurements by:

- Using L4 PTP Sync messages as probe packets. This allows it to reuse
  the packet timestamping functionalities available in many commodity
  NICs and switches.

- Scheduling transmissions according to a cyclical schedule, suitable
  for testing the performance of IEEE 802.1Qbv networks.

Similar to `iperf3(1)`, a `taping` _source_ communicates with another
`taping` _sink_. Parallel to the probe packets, a TCP control
connection is used to communicate results back to the source.


Clock synchronization
---------------------

A one-way latency is the difference between a reception timestamp
taken at the sink and a transmission timestamp taken at the source.
These are taken by two different clocks, so any offset between them
ends up directly in every sample, and any wander between them ends up
in the measured jitter. `taping` does not synchronize the clocks, it
only relies on them being synchronized:

- With hardware timestamping, the PHCs of the source and sink NICs
  must be synchronized, typically by running `ptp4l(8)` on both hosts
  in a PTP domain _other_ than the one `taping` uses.

- With software timestamping, the system clocks must be synchronized,
  e.g. by `phc2sys(8)` tracking a synchronized PHC.

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
  of the Sync message, typically in all domains. `taping` records the
  correctionField of every received probe, so the residence time can
  be separated from the propagation delay.

- Boundary clocks that are not members of `taping`'s domain will
  either forward the probes as regular traffic or drop them. `probe`
  reports which.

On the hosts themselves, `taping` binds UDP port 319, which a `ptp4l`
instance using UDP transport will already hold. Run `ptp4l` with its
L2 transport (`-2`) on hosts that also run `taping`.


HOWTO
-----

### Create a sink

First, launch a _sink_ on the host attached to the egress point of the
network under test. In simple setups, `taping` can typically be run
without any additional arguments:

    user@end:~# taping sink

In case multiple addresses are assigned to the host running the sink,
you can specify which one you want to bind to. Link-local addresses
must carry a scope:

    user@end:~# taping sink fe80::dead:1%eth0

### Test connection

Before running any measurements, ensure that the sink is reachable from
the source, that timestamps can be collected both at the source and at
the sink, and that the probes are forwarded as regular traffic:

    user@start:~# taping probe fe80::dead:1%eth0

### Source

Once the connection is known to be good, you are ready to launch a real
test. The `stat` command outputs a continuously updating summary of the
current session:

    user@start:~# taping stat fe80::dead:1%eth0
    Packets: 2043/101pps -> 2040/100pps source -> sink, 3 lost (0.1%)
    Latency: 2.350/2.390/2.468/0.054 us min/avg/max/mdev
    Tail:    2.421/2.460 us p99/p99.9

To save every sample for later analysis, use `record` instead:

    user@start:~# taping record fe80::dead:1%eth0 run1.csv


Usage
-----

```
taping [<common-options>] <command> [<command-options-and-arguments>]

taping sink   [<sink-options>] [<BIND-ADDRESS>]
taping probe  [<source-options>] <SINK-ADDRESS>
taping stat   [<source-options>] <SINK-ADDRESS>
taping record [<source-options>] <SINK-ADDRESS> [<OUTPUT-FILE>]
taping [<source-options>] <SINK-ADDRESS>

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

      Options describing a cyclical schedule, as required for 802.1Qbv
      testing, are not yet defined.

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
```

Building
--------

`taping` uses a standard autotools setup, so building essentially boils
down to:

    ./configure && make && make install

If you are building from a GIT tree and not a release tarball, you must
first generate the `configure` script:

    ./autogen.sh

Time-aware Ping
===============

`taping`, measures the forwarding latency of a network. In contrast to
`ping(8)`, it offers high precision measurements by:

- Using L4 PTP Sync messages as probe packets. This allows it to reuse
  the packet timestamping functionalities available in many commodity
  NICs and switches.

- Scheduling transmissions according to a cyclical schedule, suitable
  for testing the performance of IEEE 802.1Qbv networks.

Similar to `iperf3(1)`, a `taping` _source_ communicates with another
`taping` sink. Parallel to the probe packets, a TCP control connection
is used to communicate results back to the source.

The precision of the measured latencies depend on the variance of the
clock offset between the source and sink timestampers.


HOWTO
-----

### Create a sink

First, launch a _sink_ on the host attached to the egress point of the
network under test. In simple setups, `taping` can typically be run
without any additional arguments:

    user@end:~# taping sink

In case multiple addresses are assigned to the host running the sink,
you can specify which one you want to bind to:

    user@end:~# taping sink fe80::dead:1

### Test connection

Before running any measurements, ensure that the sink is reachable from
the source, that timestamps can be collected both at the source and at
the sink:

    user@start:~# taping probe fe80::dead:1

### Source

Once the connection is known to be good, you are ready to launch a real
test. The `stat` command outputs a continuously updating summary of the
current session:

    user@host:~# taping stat fe80::dead:1
    Packets: 2043/101pps -> 2040/100pps source -> sink
	Latency: 2.350/2.390/2.468/0.054 us min/avg/max/mdev


Usage
-----

```
taping [<common-options>] <command> [<command-options-and-arguments>]

taping sink [<ADDRESS>...]
taping probe  <SINK-ADDRESS>
taping record <SINK-ADDRESS>
taping stat   <SINK-ADDRESS>
taping [<OPTIONS>] <ADDR>

Common options:
  -T, --timestamping hw|sw|any
      hw:  Require hardware timestamping support.
	  sw:  Use software timestamping.
	  any: Use hardware timestamping if it is available, otherwise fall
	       back to software timestamping.

	  By default, hardware timestamping support is required.

  -d, --domain N
      Operate in PTP domain N. The user must ensure that the domain is
	  not used for time synchronization.

      By default, operate in domain 254.

Commands:
  sink [<sink-options>] [<bind-address>]
      Listen for connections from sources on <bind-address>, if
	  specified, otherwise from any address. Record timestamps of
	  incoming probe packets and relay the results back to the source.

  probe <source-options> <sink-address>
      Verify that the sink accepts connections. Send a single probe
	  packet, make sure that the transmission timestamp can be
	  retrieved, and that the sink relays back the reception timestamp.

  stat <source-options> <sink-address>
      Continuously send probe packets to the sink and track key
	  statistics about packets transmitted and received,
	  min/avg/max/mdev latencies, etc.

  record <source-options> <sink-address> [<output-file>]
      Record latency measurements and write the results to <output-file>
	  if specified, otherwise to stdout.

Source options:

  -c --count <n>
      Exit after sending <n> packets.

	  By default, transmission continues until it is interrupted by the
	  user.

  -d, --dscp <>|cs<n>
      Use a DSCP of <val> on probe packets.

	  By default, CS5 is used.

  -r, --rate <rate>
      Send <rate> packets per second.

	  By default, 100 pps are sent.

  -s, --size <size>
      Use probe packets that are <size> bytes long.

	  By defaults, 64 byte packets are used.
```

Building
--------

`taping` uses a standard autotools setup, so building essentially boils
down to:

    ./configure && make && make install

If you are building from a GIT tree and not a release tarball, you must
first generate the `configure` script:

    ./autogen.sh


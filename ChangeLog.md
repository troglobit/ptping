Change Log
==========

All notable changes to the project are documented in this file.

[v0.1][] - 2026-11-10
---------------------

First release.  `ptping` measures the forwarding latency of a network
with PTP Sync messages as probes, so that it can reuse the timestamping
of commodity NICs and switches.  Licensed under the GNU General Public
License, version 2.

### Changes

- `probe`, `stat` and `record` commands: send one probe and report its
  fate, show a live latency table, or write one CSV line per sample
- `sink` command: answers sources over a TCP control connection and
  returns the reception timestamps
- Hardware or software timestamping, chosen by the source with
  `--timestamping`
- `--bidir` sends probes in the reverse direction as well, as a check on
  clock synchronization and path asymmetry
- Cyclic transmission schedule for 802.1Qbv networks: cycle time,
  window, base time and optional launch time for the `etf` qdisc.
  `--observe` sends throughout the cycle and tags each probe by where
  it left
- Time options take units (`ns`, `us`, `ms`, `s`); `--rate` takes a rate
  or the time between probes
- Several streams from one source, one per sink address
- `contrib/ptping-view`, a browser for recordings

[v0.1]: https://github.com/wkz/ptping/releases/tag/v0.1

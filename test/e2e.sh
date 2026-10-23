#!/bin/sh
# End-to-end test with software timestamps. As root, the sink and the
# source run in two network namespaces joined by a veth pair; otherwise
# both run on loopback with distinct addresses and an unprivileged port.

set -e

ptping=${PTPING:-./ptping}
tmp=$(mktemp -d)
sink_pid=

cleanup()
{
	[ -n "$sink_pid" ] && kill -INT "$sink_pid" 2>/dev/null && wait "$sink_pid" || true
	if [ -n "$src_ns" ]; then
		ip netns del ptping-a 2>/dev/null || true
		ip netns del ptping-b 2>/dev/null || true
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

# run <name> <command...>: run a source command, keep its output in
# $tmp/<name>.log, and fail with that output if it fails.
run()
{
	name=$1
	shift
	$src_ns $ptping $common "$@" >"$tmp/$name.log" 2>&1 ||
		fail "$name: $(cat "$tmp/$name.log")"
}

# refuse <name> <pattern> <command...>: the command must fail and
# explain itself with a message matching <pattern>.
refuse()
{
	name=$1
	pattern=$2
	shift 2
	$src_ns $ptping $common "$@" >"$tmp/$name.log" 2>&1 &&
		fail "$name: accepted"
	grep -q "$pattern" "$tmp/$name.log" ||
		fail "$name: $(cat "$tmp/$name.log")"
}

if [ "$(id -u)" = 0 ]; then
	ip netns add ptping-a
	ip netns add ptping-b
	ip link add veth-a netns ptping-a type veth peer name veth-b netns ptping-b
	ip -n ptping-a addr add 10.0.0.1/24 dev veth-a
	ip -n ptping-b addr add 10.0.0.2/24 dev veth-b
	ip -n ptping-a link set veth-a up
	ip -n ptping-b link set veth-b up
	ip -n ptping-a link set lo up
	ip -n ptping-b link set lo up
	sink_ns="ip netns exec ptping-b"
	src_ns="ip netns exec ptping-a"
	sink=10.0.0.2
	common="-T sw -p 319"
else
	sink_ns=
	src_ns=
	sink=127.0.0.2
	common="-T sw -p 31900"
fi

$sink_ns $ptping $common sink $sink >"$tmp/sink.log" 2>&1 &
sink_pid=$!
sleep 0.3

echo "probe"
run probe probe -b $sink
grep -q "source -> sink: latency" "$tmp/probe.log" || fail "probe: no forward latency"
grep -q "sink -> source: latency" "$tmp/probe.log" || fail "probe: no reverse latency"

echo "stat"
run stat stat -b -c 100 -r 200 -i 200ms $sink
tail -8 "$tmp/stat.log" | grep -q "^source -> sink: 100 sent.*100 received.* 0 lost (0.0%)$" || fail "stat: $(tail -8 "$tmp/stat.log")"

echo "record"
run record record -o "$tmp/rec.csv" -c 20 -r 200 $sink
[ "$(wc -l <"$tmp/rec.csv")" = 21 ] || fail "record: $(wc -l <"$tmp/rec.csv") lines"
[ "$(head -1 "$tmp/rec.csv")" = "stream,flow,seq,tx_ns,rx_ns,correction,latency_ns,tx_phase_ns,rx_phase_ns,cycles,tag" ] || fail "record: header"
missing=$(cut -d, -f1-7 "$tmp/rec.csv" | grep -E ",,|,$" || true)
[ -z "$missing" ] || fail "record: missing fields: $missing"
[ "$(tail -1 "$tmp/rec.csv" | cut -d, -f1-3)" = "$sink,fwd,19" ] || fail "record: last line $(tail -1 "$tmp/rec.csv")"

echo "window"
# 50 ms cycle, open for its first half, probes every 2 ms: ten per
# cycle, the last due 18 ms in. Unprivileged, nothing keeps the CPU
# out of idle states that take a millisecond to leave, so the window
# leaves that much margin.
run window stat -b -c 20 -r 500 --cycle 50ms --window 0-25ms $sink
tail -12 "$tmp/window.log" | grep -q "^source -> sink: 20 sent.*20 received.* 0 lost" || fail "window: $(tail -12 "$tmp/window.log")"
[ "$(tail -12 "$tmp/window.log" | grep -c "^inside  *20 ")" = 2 ] || fail "window: not all inside: $(tail -12 "$tmp/window.log")"
run winrec record -o "$tmp/win.csv" -c 20 -r 2ms --cycle 50ms --window 0+25ms $sink
bad=$(tail -n +2 "$tmp/win.csv" | awk -F, '$8 == "" || $8 < 0 || $8 >= 25000000')
[ -z "$bad" ] || fail "window: phases outside: $bad"

echo "observe"
# Sent throughout a 50 ms cycle at a period that does not divide it,
# every probe is tagged by where it left relative to the window.
# Loopback has no gate, so a probe that left inside arrives inside,
# and none crosses a cycle boundary.
run observe stat -c 30 -r 333 --cycle 0.05s --window 10000us+20ms --observe $sink
tail -8 "$tmp/observe.log" | grep -q "^outside  *[1-9]" || fail "observe: $(tail -8 "$tmp/observe.log")"
run obsrec record -o "$tmp/obs.csv" -c 30 -r 333.3hz --cycle 50ms --window 10ms-30ms --observe $sink
bad=$(tail -n +2 "$tmp/obs.csv" | awk -F, '$10 != 0 || ($11 != "inside" && $11 != "outside") || ($11 == "inside" && ($9 < 10000000 || $9 >= 30000000))')
[ -z "$bad" ] || fail "observe: $bad"
n=$(tail -n +2 "$tmp/obs.csv" | awk -F, '$11 == "inside"' | wc -l)
[ "$n" -gt 5 ] && [ "$n" -lt 25 ] || fail "observe: $n of 30 inside"

echo "streams"
# Each address takes the options before it. The two bidirectional
# streams share a sink, so their reverse probes are told apart by the
# identity the sink gives each session.
run streams stat -c 50 -r 500 -b -n a -d cs7 $sink -n b -d 0 $sink --no-bidir $sink
for s in "a +source -> sink" "a +sink -> source" "b +source -> sink" "b +sink -> source" "$sink +source -> sink"; do
	tail -20 "$tmp/streams.log" | grep -Eq "^$s: 50 sent.*50 received.* 0 lost" ||
		fail "streams: $s: $(tail -20 "$tmp/streams.log")"
done
tail -20 "$tmp/streams.log" | grep -Eq "^$sink +sink -> source" && fail "streams: --no-bidir"
run strrec record -o "$tmp/streams.csv" -c 10 -r 500 -b $sink $sink
[ "$(tail -n +2 "$tmp/streams.csv" | cut -d, -f1,2 | sort | uniq -c | awk '{ print $1, $2 }' | tr '\n' ' ')" = \
  "10 $sink#2,fwd 10 $sink#2,rev 10 $sink,fwd 10 $sink,rev " ] || fail "strrec: $(cat "$tmp/streams.csv")"
run strprobe probe -b -n x $sink -n y $sink
[ "$(grep -c "^[xy]: .*: latency" "$tmp/strprobe.log")" = 4 ] || fail "strprobe: $(cat "$tmp/strprobe.log")"

echo "refusal"
refuse size "smallest frame" stat -c 1 -s 50 $sink
refuse hw "no hardware timestamping" -T hw stat -c 1 $sink
refuse trailing "none after the last" stat $sink -c 1
refuse names "two streams are named" stat -c 1 -n a $sink -n a $sink

echo "sink log:"
cat "$tmp/sink.log"

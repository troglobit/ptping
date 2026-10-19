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
run record record -c 20 -r 200 $sink "$tmp/rec.csv"
[ "$(wc -l <"$tmp/rec.csv")" = 21 ] || fail "record: $(wc -l <"$tmp/rec.csv") lines"
[ "$(head -1 "$tmp/rec.csv")" = "flow,seq,tx_ns,rx_ns,correction,latency_ns,tx_phase_ns,rx_phase_ns,cycles,tag" ] || fail "record: header"
missing=$(cut -d, -f1-6 "$tmp/rec.csv" | grep -E ",,|,$" || true)
[ -z "$missing" ] || fail "record: missing fields: $missing"
[ "$(tail -1 "$tmp/rec.csv" | cut -d, -f1,2)" = "fwd,19" ] || fail "record: last line $(tail -1 "$tmp/rec.csv")"

echo "window"
# 50 ms cycle, open for its first half, probes every 2 ms: ten per
# cycle, the last due 18 ms in. Unprivileged, nothing keeps the CPU
# out of idle states that take a millisecond to leave, so the window
# leaves that much margin.
run window stat -b -c 20 -r 500 --cycle 50ms --window 0-25ms $sink
tail -12 "$tmp/window.log" | grep -q "^source -> sink: 20 sent.*20 received.* 0 lost" || fail "window: $(tail -12 "$tmp/window.log")"
[ "$(tail -12 "$tmp/window.log" | grep -c "^inside  *20 ")" = 2 ] || fail "window: not all inside: $(tail -12 "$tmp/window.log")"
run winrec record -c 20 -r 500 --cycle 50ms --window 0+25ms $sink "$tmp/win.csv"
bad=$(tail -n +2 "$tmp/win.csv" | awk -F, '$7 == "" || $7 < 0 || $7 >= 25000000')
[ -z "$bad" ] || fail "window: phases outside: $bad"

echo "observe"
# Sent throughout a 50 ms cycle at a period that does not divide it,
# every probe is tagged by where it left relative to the window.
# Loopback has no gate, so a probe that left inside arrives inside,
# and none crosses a cycle boundary.
run observe stat -c 30 -r 333 --cycle 0.05s --window 10000us+20ms --observe $sink
tail -8 "$tmp/observe.log" | grep -q "^outside  *[1-9]" || fail "observe: $(tail -8 "$tmp/observe.log")"
run obsrec record -c 30 -r 333 --cycle 50ms --window 10ms-30ms --observe $sink "$tmp/obs.csv"
bad=$(tail -n +2 "$tmp/obs.csv" | awk -F, '$9 != 0 || ($10 != "inside" && $10 != "outside") || ($10 == "inside" && ($8 < 10000000 || $8 >= 30000000))')
[ -z "$bad" ] || fail "observe: $bad"
n=$(tail -n +2 "$tmp/obs.csv" | awk -F, '$10 == "inside"' | wc -l)
[ "$n" -gt 5 ] && [ "$n" -lt 25 ] || fail "observe: $n of 30 inside"

echo "refusal"
refuse size "smallest frame" stat -c 1 -s 50 $sink
refuse hw "no hardware timestamping" -T hw stat -c 1 $sink

echo "sink log:"
cat "$tmp/sink.log"

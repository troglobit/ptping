AM_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
AM_CPPFLAGS = -I$(top_srcdir)/src -D_GNU_SOURCE

noinst_LIBRARIES = libtaping.a
libtaping_a_SOURCES = \
	src/be.h \
	src/ctrl.c \
	src/ctrl.h \
	src/flow.c \
	src/flow.h \
	src/log.c \
	src/log.h \
	src/netif.c \
	src/netif.h \
	src/ptp.c \
	src/ptp.h \
	src/sched.h \
	src/stats.c \
	src/stats.h \
	src/ts.c \
	src/ts.h \
	src/tspec.h

bin_PROGRAMS = taping
taping_SOURCES = src/probe.c src/record.c src/sink.c src/source.c src/source.h src/stat.c src/taping.c src/taping.h

check_PROGRAMS = test/ctrl-test test/flow-test test/netif-test test/ptp-test test/stats-test test/ts-test
TESTS = $(check_PROGRAMS)
LDADD = libtaping.a -lm

test_ctrl_test_SOURCES = test/ctrl-test.c test/test.h
test_flow_test_SOURCES = test/flow-test.c test/test.h
test_netif_test_SOURCES = test/netif-test.c test/test.h
test_ptp_test_SOURCES = test/ptp-test.c test/test.h
test_stats_test_SOURCES = test/stats-test.c test/test.h
test_ts_test_SOURCES = test/ts-test.c test/test.h

EXTRA_DIST = autogen.sh README.md doc/architecture.md

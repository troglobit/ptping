AM_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter
AM_CPPFLAGS = -I$(top_srcdir)/src

noinst_LIBRARIES = libtaping.a
libtaping_a_SOURCES = \
	src/be.h \
	src/log.h \
	src/ptp.c \
	src/ptp.h \
	src/stats.c \
	src/stats.h \
	src/tspec.h

bin_PROGRAMS = taping
taping_SOURCES = src/taping.c src/taping.h

check_PROGRAMS = test/ptp-test test/stats-test
TESTS = $(check_PROGRAMS)
LDADD = libtaping.a -lm

test_ptp_test_SOURCES = test/ptp-test.c test/test.h
test_stats_test_SOURCES = test/stats-test.c test/test.h

EXTRA_DIST = autogen.sh README.md doc/architecture.md

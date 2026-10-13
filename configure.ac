AC_INIT([taping], [0.1], [tobias@waldekranz.com])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_SRCDIR([src/taping.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE([1.11 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

AC_PROG_CC
AC_USE_SYSTEM_EXTENSIONS

AC_CHECK_HEADERS([linux/net_tstamp.h linux/ptp_clock.h linux/sockios.h], [],
	[AC_MSG_ERROR([Linux timestamping headers are required])])

AC_CHECK_DECL([SOF_TIMESTAMPING_OPT_CMSG], [],
	[AC_MSG_ERROR([kernel headers are too old])],
	[[#include <linux/net_tstamp.h>]])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

#ifndef PTPING_LOG_H
#define PTPING_LOG_H

#include <stdio.h>
#include <stdlib.h>

extern int log_level;

enum {
	LOG_ERR,
	LOG_WARN,
	LOG_INFO,
	LOG_DBG,
};

#define log_at(_lvl, _pfx, _fmt, ...) do {				\
	if (log_level >= (_lvl))					\
		fprintf(stderr, _pfx _fmt "\n", ##__VA_ARGS__);		\
} while (0)

#define err(...)  log_at(LOG_ERR,  "error: ", __VA_ARGS__)
#define warn(...) log_at(LOG_WARN, "warning: ", __VA_ARGS__)
#define info(...) log_at(LOG_INFO, "", __VA_ARGS__)
#define dbg(...)  log_at(LOG_DBG,  "debug: ", __VA_ARGS__)

#define die(...) do { err(__VA_ARGS__); exit(1); } while (0)

/* Like perror(3), but through the log and with a format string. */
#define err_errno(_fmt, ...) err(_fmt ": %m", ##__VA_ARGS__)
#define die_errno(_fmt, ...) die(_fmt ": %m", ##__VA_ARGS__)

#endif

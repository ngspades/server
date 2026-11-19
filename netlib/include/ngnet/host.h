/*
	ngnet: the connection layer, a library inside the game's repository
	until it gets a repository of its own (netlib/README.md).

	What the library needs from the program that links it, and nothing else:
	a place to write its log lines and events. The game calls ngnet_set_log() once at
	start-up; a program that never does gets the lines on stderr.
*/

#ifndef NGNET_HOST_H
#define NGNET_HOST_H

enum {
	NGNET_LOG_INFO,
	NGNET_LOG_WARN,
	NGNET_LOG_ERROR,
};

/* one finished line, no newline */
typedef void (*ngnet_log_fn)(int level, const char* line);

/* NULL puts the default (stderr) back */
void ngnet_set_log(ngnet_log_fn fn);

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void ngnet_log(int level, const char* fmt, ...);

#define ngnet_info(...) ngnet_log(NGNET_LOG_INFO, __VA_ARGS__)
#define ngnet_warn(...) ngnet_log(NGNET_LOG_WARN, __VA_ARGS__)
#define ngnet_error(...) ngnet_log(NGNET_LOG_ERROR, __VA_ARGS__)

/* a structured event ("net", "action=resolve host=x"): what a test reads
   instead of the log's prose. Dropped when no program asked for them */
typedef void (*ngnet_event_fn)(const char* name, const char* fields);
void ngnet_set_event(ngnet_event_fn fn);

#ifdef __GNUC__
__attribute__((format(printf, 2, 3)))
#endif
void ngnet_event(const char* name, const char* fmt, ...);

#endif

/* The library's log hook (include/ngnet/host.h). */

#include <stdarg.h>
#include <stdio.h>

#include "ngnet/host.h"

static ngnet_log_fn sink;

void ngnet_set_log(ngnet_log_fn fn) {
	sink = fn;
}

void ngnet_log(int level, const char* fmt, ...) {
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if(sink)
		sink(level, line);
	else
		fprintf(stderr, "ngnet: %s\n", line);
}

static ngnet_event_fn event_sink;

void ngnet_set_event(ngnet_event_fn fn) {
	event_sink = fn;
}

void ngnet_event(const char* name, const char* fmt, ...) {
	char fields[512];
	va_list ap;
	if(!event_sink)
		return;
	va_start(ap, fmt);
	vsnprintf(fields, sizeof(fields), fmt, ap);
	va_end(ap);
	event_sink(name, fields);
}

#ifndef RECOVERY_CRASH_H
#define RECOVERY_CRASH_H

#include <signal.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "input.h"
#include "ui.h"

/* The command line of the CrashReport plugin, run on a pseudo terminal. */
#define CRASHREPORT "/usr/bin/crashreport"

/* The newest crash or debug log of enigma2 in its log folders, 0 when there is none. */
int crash_log_path(char *path, size_t size);
int debug_log_path(char *path, size_t size);
int crash_missing_module(char *module, size_t size);

struct log_file {
	char path[512];
	time_t time;
	long long size;
	dev_t device;
	ino_t inode;
};

/* The crash logs of enigma2 in its log folders, the newest first, at most max. Returns their count. */
int crash_logs(struct log_file *logs, int max);

void crash_report(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop);

#endif

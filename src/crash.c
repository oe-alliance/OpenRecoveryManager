#define _GNU_SOURCE

#include "crash.h"

#include "boxinfo.h"
#include "console.h"
#include "i18n.h"
#include "viewer.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE _("Crash report")

/* config.crash.debug_path, enigma2 saves only a changed value. */
static void debug_path(char *path, size_t size)
{
	static const char key[] = "config.crash.debug_path=";
	char line[256];
	FILE *file = fopen("/etc/enigma2/settings", "r");
	snprintf(path, size, "/home/root/logs/");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
		if (strncmp(line, key, sizeof(key) - 1) == 0) {
			text_cut(line, "\r\n");
			snprintf(path, size, "%.200s", line + sizeof(key) - 1);
		}
	fclose(file);
}

static int is_crash_log(const char *name)  /* Like the names of main/bsod.cpp. */
{
	size_t length = strlen(name);
	return (length > 18 && strcmp(name + length - 18, "-enigma2-crash.log") == 0) ||
		(strncmp(name, "enigma2_crash", 13) == 0 && length > 4 && strcmp(name + length - 4, ".log") == 0);
}

/* Keeps logs sorted, the newest first; the same file in two folders only once. */
static int add_log(struct log_file *logs, int count, int max, const char *path, const struct stat *info)
{
	int at;
	for (at = 0; at < count; ++at)
		if (logs[at].device == info->st_dev && logs[at].inode == info->st_ino)
			return count;
	for (at = 0; at < count && logs[at].time >= info->st_mtime; ++at)
		;
	if (at >= max)
		return count;
	if (count == max)
		--count;
	memmove(&logs[at + 1], &logs[at], (size_t)(count - at) * sizeof(*logs));
	snprintf(logs[at].path, sizeof(logs[at].path), "%s", path);
	logs[at].time = info->st_mtime;
	logs[at].size = (long long)info->st_size;
	logs[at].device = info->st_dev;
	logs[at].inode = info->st_ino;
	return count + 1;
}

/* The output of ORM itself, e.g. the debug log enigma2.sh writes when ORM runs instead of enigma2. */
static int own_output(const struct stat *info)
{
	struct stat out;
	for (int fd = STDOUT_FILENO; fd <= STDERR_FILENO; ++fd)
		if (fstat(fd, &out) == 0 && out.st_dev == info->st_dev && out.st_ino == info->st_ino)
			return 1;
	return 0;
}

static int list_logs(int (*matches)(const char *), struct log_file *logs, int max)
{
	char folders[3][256];
	int count = 0;
	debug_path(folders[0], sizeof(folders[0]));
	snprintf(folders[1], sizeof(folders[1]), "/home/root/logs/");
	snprintf(folders[2], sizeof(folders[2]), "/tmp/");
	for (int i = 0; i < 3; ++i) {
		DIR *dir = folders[i][0] ? opendir(folders[i]) : NULL;  /* NOSONAR the log folders of enigma2 */
		struct dirent *entry;
		if (!dir)
			continue;
		while ((entry = readdir(dir))) {
			char candidate[512];
			struct stat info;
			if (!matches(entry->d_name))
				continue;
			snprintf(candidate, sizeof(candidate), "%.250s%s%.250s", folders[i],
				folders[i][strlen(folders[i]) - 1] == '/' ? "" : "/", entry->d_name);
			if (lstat(candidate, &info) == 0 && S_ISREG(info.st_mode) && !own_output(&info))
				count = add_log(logs, count, max, candidate, &info);
		}
		closedir(dir);
	}
	return count;
}

static int newest_log(int (*matches)(const char *), char *path, size_t size)
{
	struct log_file newest;
	if (!list_logs(matches, &newest, 1))
		return 0;
	snprintf(path, size, "%s", newest.path);
	return 1;
}

static int is_debug_log(const char *name)
{
	size_t length = strlen(name);
	return length > 18 && strcmp(name + length - 18, "-enigma2-debug.log") == 0;
}

int crash_log_path(char *path, size_t size)
{
	return newest_log(is_crash_log, path, size);
}

int debug_log_path(char *path, size_t size)
{
	return newest_log(is_debug_log, path, size);
}

int crash_logs(struct log_file *logs, int max)
{
	return list_logs(is_crash_log, logs, max);
}

struct report {
	struct console console;
	char context[2048];  /* What crashreport printed before its question. */
	char status[256];
	char tracking[64];
	int answered;
};

static void handle_line(const struct console *console, const char *line)
{
	struct report *r = console->data;
	size_t used = strlen(r->context);
	if (!line[0])
		return;
	if (strncmp(line, "Tracking number: ", 17) == 0)
		snprintf(r->tracking, sizeof(r->tracking), "%s", line + 17);
	if (!r->answered)
		snprintf(r->context + used, sizeof(r->context) - used, "%s%s", used ? "\n" : "", line);
	snprintf(r->status, sizeof(r->status), "%.255s", line);
}

static void handle_question(const struct console *console, const char *question, int seconds)
{
	struct report *r = console->data;
	char body[2600];
	snprintf(body, sizeof(body), "%s\n\n%s", r->context, question);
	r->answered = 1;
	console_ask(console, TITLE, body, seconds);
}

static void wait_key(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
}

/* Only the crash log by default, the debug log and the diagnostics follow the settings of the plugin. */
static int choose_logs(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop)
{
	const char *items[2];
	char footer[128];
	char distro[64];
	char question[256];
	int selected = 0;
	boxinfo_value("displaydistro", distro, sizeof(distro));
	while (!(stop && *stop)) {
		enum input_key key;
		items[0] = _("Crash log only");
		items[1] = _("Crash log and more details (recommended)");
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Confirm"),
			.back = _("Menu")});
		/* TRANSLATORS: %s is the distribution, e.g. OpenATV. */
		snprintf(question, sizeof(question), _("Which information should be sent to the %s ticket system? You see "
			"everything before it is sent."), distro[0] ? distro : "OpenATV");
		ui_menu(ui, TITLE, question, items, 2, selected, footer);
		key = input_next(input, 1000);
		selected = list_move(key, selected, 2);
		if (key == INPUT_OK)
			return selected;
		else if (key == INPUT_BACK || key == INPUT_RED)
			return -1;
	}
	return -1;
}

void crash_report(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	static char *const only_crash[] = {"crashreport", "--no-debug", "--no-diagnostics", NULL};
	static char *const everything[] = {"crashreport", NULL};
	struct report *r;
	char footer[64];
	int choice;
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = _("Menu")});
	if (access(CRASHREPORT, X_OK) != 0) {
		ui_error(ui, TITLE, _("Crash reports are not installed. The CrashReport plugin provides them."));
		wait_key(input, stop);
		return;
	}
	choice = choose_logs(ui, input, stop);
	if (choice < 0)
		return;
	r = calloc(1, sizeof(*r));
	if (!r)
		return;
	r->console = (struct console){.ui = ui, .input = input, .stop = stop, .master = -1,
		.line = handle_line, .question = handle_question, .data = r};
	if (!console_start(&r->console, CRASHREPORT, choice ? everything : only_crash)) {
		ui_error(ui, TITLE, _("crashreport cannot be started."));
		wait_key(input, stop);
		console_close(&r->console);
		free(r);
		return;
	}
	while (!console_poll(&r->console, 300))
		ui_progress(ui, TITLE, r->answered ? _("Sending the crash report.") : _("Preparing the crash report."),
			50, r->status[0] ? r->status : _("Please wait..."), _("Please wait..."));
	if (r->console.link[0]) {
		char body[512];
		snprintf(body, sizeof(body), _("Tracking number: %s\n\nScan the QR code or open the link on a phone or PC. "
			"Sign in and describe what happened, so the developers can help. The report stays open for 48 hours."),
			r->tracking[0] ? r->tracking : _("unknown"));
		ui_remote_session(ui, &(struct ui_remote_page){.title = TITLE, .body = body, .link = r->console.link,
			.qrcode = r->console.has_qrcode ? r->console.qrcode : NULL, .footer = footer});
	} else if (!r->status[0] || strncmp(r->status, "Error: ", 7) == 0)
		ui_error(ui, TITLE, r->status[0] ? r->status + 7 : _("The crash report was not sent."));
	else
		ui_screen(ui, TITLE, r->status, footer);  /* E.g. nothing was sent after no. */
	wait_key(input, stop);
	console_close(&r->console);
	free(r);
}

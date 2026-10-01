#define _GNU_SOURCE

#include "about.h"
#include "backup.h"
#include "boxinfo.h"
#include "crash.h"
#include "flash.h"
#include "i18n.h"
#include "input.h"
#include "language.h"
#include "plugins.h"
#include "remote.h"
#include "reset.h"
#include "slots.h"
#include "update.h"
#include "viewer.h"
#include "ui.h"
#include "version.h"
#include "watch.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The exit codes tell enigma2.sh what to do, like the ones of enigma2. */
#define ACTION_START 0
#define ACTION_POWER_OFF 1
#define ACTION_REBOOT 2
#define ACTION_CRASH_LOG 102  /* Stays in the menu, like the ones after it. */
#define ACTION_CRASH_REPORT 103
#define ACTION_SLOT 104
#define ACTION_UPDATE 105
#define ACTION_PLUGINS 106
#define ACTION_RESET 107
#define ACTION_FLASH 108
#define ACTION_BACKUP 109

#define LOG_MAX_LINES 4000
#define BLUESCREEN_LINES 20  /* The end of the log that main/bsod.cpp shows. */

#define COUNTDOWN_SECONDS 15

static volatile sig_atomic_t stop_requested;
static int blue_taken;  /* BLUE belongs to the screen, not to Remote Support. */
static int global_pressed;  /* HELP, BLUE or INFO, which stop the countdown too. */
static int languages;  /* HELP is there: another language than English is installed. */
static int in_entry;  /* The screen of an entry has the keys. */
static int jump_to = -1;  /* The entry a digit chose there, opened once back in the menu. */
static char item_marks[16];  /* Grey entries, which digits do not open. */
static int from_shell;  /* Started by hand, no start script acts on the exit code. */

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

/* What happened for the card of the menu: its title and the last step, below it the signal. Without a
 * result ORM was started on request. */
static void describe(const struct watch_result *result, char *title, size_t title_size, char *text,
	size_t text_size)
{
	char detail[96] = "";
	if (!result) {
		snprintf(title, title_size, "%s", _("REQUESTED"));
		snprintf(text, text_size, "%s\n%s", _("Enigma2 is stopped"), _("The Open Recovery Manager was started on request."));
		return;
	}
	if (strcmp(result->reason, "hang") == 0)
		snprintf(title, title_size, "%s", _("ENIGMA2 STOPPED RESPONDING WHILE STARTING"));
	else if (strcmp(result->crash, "python") == 0)
		snprintf(title, title_size, "%s", result->ready ? _("ENIGMA2 STOPPED WITH A PYTHON ERROR SHORTLY AFTER THE START") :
			_("ENIGMA2 STOPPED WITH A PYTHON ERROR WHILE STARTING"));
	else
		snprintf(title, title_size, "%s", result->ready ? _("ENIGMA2 CRASHED SHORTLY AFTER THE START") :
			_("ENIGMA2 CRASHED WHILE STARTING"));
	if (result->crash[0] && strcmp(result->crash, "python") != 0) {
		const char *name = watch_signal_name(result->crash);
		detail[0] = '\n';
		snprintf(detail + 1, sizeof(detail) - 1, _("Signal %s"), name ? name : result->crash);
	}
	snprintf(text, text_size, _("Last step: %s"), result->step[0] ? result->step : _("unknown"));
	snprintf(text + strlen(text), text_size - strlen(text), "%s", detail);
}

static void free_lines(char **lines, int count)
{
	for (int i = 0; i < count; ++i)
		free(lines[i]);
	free(lines);
}

/* The last LOG_MAX_LINES lines of a file. */
static int load_lines(const char *path, char ***out)
{
	char buffer[512];
	char **lines = calloc(LOG_MAX_LINES, sizeof(*lines));
	int count = 0;
	FILE *file;
	*out = lines;
	if (!lines)
		return 0;
	file = fopen(path, "r");  /* NOSONAR a log of enigma2 */
	if (!file)
		return 0;
	while (fgets(buffer, sizeof(buffer), file)) {
		text_cut(buffer, "\r\n");
		if (count == LOG_MAX_LINES) {
			free(lines[0]);
			memmove(lines, lines + 1, (LOG_MAX_LINES - 1) * sizeof(*lines));
			count--;
		}
		lines[count++] = strdup(buffer);
	}
	fclose(file);
	return count;
}

/* "time  message" becomes "time\tduration\tmessage"; a step lasts until the next line. */
static void add_durations(char **lines, int count)
{
	for (int i = 0; i < count; ++i) {
		char duration[16] = "";
		char *line = lines[i];
		char *message;
		double time;
		double next;
		size_t size;
		if (!line || sscanf(line, "%lf", &time) != 1)
			continue;
		message = line + strspn(line, " ");
		message += strcspn(message, " ");
		message += strspn(message, " ");
		if ((strncmp(message, "step ", 5) == 0 || strncmp(message, "watching ", 9) == 0) &&
			i + 1 < count && lines[i + 1] && sscanf(lines[i + 1], "%lf", &next) == 1)
			snprintf(duration, sizeof(duration), "%.3f", next - time);
		size = strlen(message) + 48;
		lines[i] = malloc(size);
		if (!lines[i]) {
			lines[i] = line;
			continue;
		}
		snprintf(lines[i], size, "%.3f\t%s\t%s", time, duration, message);
		free(line);
	}
}

/* The lines broken at spaces where they are wider than the text view. */
static int wrap_lines(char ***lines, int count)
{
	char **wrapped = NULL;
	int total = 0;
	for (int i = 0; i < count; ++i) {
		const char *rest = (*lines)[i] ? (*lines)[i] : "";
		do {
			size_t skip;
			size_t fit = text_break(rest, &skip);
			char **grown;
			if (!(grown = realloc(wrapped, (size_t)(total + 1) * sizeof(*wrapped))))
				break;
			wrapped = grown;
			wrapped[total++] = strndup(rest, fit);
			rest += fit + skip;
		} while (*rest);
	}
	free_lines(*lines, count);
	*lines = wrapped;
	return total;
}

#define MAX_CRASH_LOGS 100

/* The crash logs, the newest first; OK returns the one chosen, BACK -1. */
static int choose_crash_log(const struct ui_context *ui, struct input_context *input, const struct log_file *logs,
	int count, int current)
{
	char (*rows)[640] = malloc((size_t)count * sizeof(*rows));
	const char **items = malloc((size_t)count * sizeof(*items));
	char title[96];
	char header[96];
	char footer[160];
	int selected = current;
	int chosen = -1;
	if (!rows || !items) {
		free(rows);
		free(items);
		return -1;
	}
	for (int i = 0; i < count; ++i) {
		char date[32] = "";
		char name[320];
		const char *file = strrchr(logs[i].path, '/') ? strrchr(logs[i].path, '/') + 1 : logs[i].path;
		struct tm when;
		if (localtime_r(&logs[i].time, &when))
			strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &when);
		if (i == current)
			snprintf(name, sizeof(name), _("%s (current)"), file);
		else
			snprintf(name, sizeof(name), "%s", file);
		snprintf(rows[i], sizeof(rows[i]), "%s\t%s\t%lld KB", date, name, (logs[i].size + 1023) / 1024);
		items[i] = rows[i];
	}
	snprintf(title, sizeof(title), _("Crash logs (%d)"), count);
	snprintf(header, sizeof(header), "%s\t%s\t%s", _("Date"), _("File"), _("Size"));
	while (!stop_requested) {
		enum input_key key;
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Show"),
			.back = _("Back")});
		ui_menu_table(ui, &(struct ui_menu){.title = title,
			.body = _("Which crash log should be shown? The newest is at the top."), .header = header, .items = items,
			.count = count, .selected = selected, .marked = -1, .align = "llr", .footer = footer});
		key = input_next(input, 1000);
		selected = list_move(key, selected, count);
		if (key == INPUT_OK)
			chosen = selected;
		if (key == INPUT_OK || key == INPUT_BACK)
			break;
	}
	free(rows);
	free(items);
	return chosen;
}

/* The line with four spaces for each tab. */
static char *expand_tabs(const char *line)
{
	char *spaced = malloc(strlen(line) * 4 + 1);
	char *out = spaced;
	if (!spaced)
		return NULL;
	for (const char *in = line; *in; ++in)
		if (*in == '\t') {
			memcpy(out, "    ", 4);
			out += 4;
		} else
			*out++ = *in;
	*out = '\0';
	return spaced;
}

/* A crash log for the text view, at first the end of its log like on the old blue screen. */
static int load_crash_log(const char *path, char ***lines, int *first, char *title, size_t title_size)
{
	int count = path[0] ? load_lines(path, lines) : 0;
	int log_lines = count;  /* Before they are broken. */
	*first = 0;
	for (int i = 0; i < count; ++i) {
		const char *tab = (*lines)[i] ? strchr((*lines)[i], '\t') : NULL;
		if (tab) {  /* A tab separates columns in the text view. */
			char *spaced = expand_tabs((*lines)[i]);
			if (!spaced)
				continue;
			free((*lines)[i]);
			(*lines)[i] = spaced;
		}
	}
	count = wrap_lines(lines, count);
	for (int i = 0; i < count; ++i)
		if (strcmp((*lines)[i] ? (*lines)[i] : "", "dmesg") == 0)  /* The kernel log follows the log of enigma2. */
			*first = i - BLUESCREEN_LINES;
	if (count && !*first)
		*first = count - BLUESCREEN_LINES;
	if (*first < 0)
		*first = 0;
	snprintf(title, title_size, ngettext("Crash log, %s (%d line)", "Crash log, %s (%d lines)", log_lines),
		strrchr(path, '/') ? strrchr(path, '/') + 1 : _("none"), log_lines);
	return count;
}

/* The newest crash log, YELLOW chooses another one.
 * BLUE switches to the messages enigma2 sent to the socket of ORM at its last start. */
static void show_logs(const struct ui_context *ui, struct input_context *input)
{
	struct log_file *logs = malloc(MAX_CRASH_LOGS * sizeof(*logs));
	char title[192];
	char watch_title[128];
	char watch_header[96];
	char crash_footer[160];
	char watch_footer[160];
	char **lines = NULL;
	char **watch_lines = NULL;
	int log_count = logs ? crash_logs(logs, MAX_CRASH_LOGS) : 0;
	int current = 0;
	int first;
	int count = load_crash_log(log_count ? logs[0].path : "", &lines, &first, title, sizeof(title));
	int watch_count = load_lines(WATCH_LOG, &watch_lines);
	int watch_first = 0;
	int socket_log = 0;
	add_durations(watch_lines, watch_count);
	snprintf(watch_title, sizeof(watch_title), ngettext("Socket log (%d line)", "Socket log (%d lines)", watch_count),
		watch_count);
	snprintf(watch_header, sizeof(watch_header), "%s\t%s\t%s", _("Time"), _("Duration"), _("Message"));
	ui_keys(crash_footer, sizeof(crash_footer), &(struct ui_key_names){.arrows = _("Scroll"),
		.yellow = log_count > 1 ? _("Switch log") : NULL, .blue = _("Socket log"), .back = _("Menu")});
	ui_keys(watch_footer, sizeof(watch_footer), &(struct ui_key_names){.arrows = _("Scroll"), .blue = _("Crash log"),
		.back = _("Menu")});
	blue_taken = 1;
	for (;;) {
		enum input_key key = socket_log ?
			text_view(ui, input, &stop_requested, &(struct text_page){.title = watch_title, .header = watch_header,
				.lines = watch_lines, .count = watch_count, .first = &watch_first, .align = "rr",
				.empty = _("There is no log of the last start."), .footer = watch_footer}) :
			text_view(ui, input, &stop_requested, &(struct text_page){.title = title, .lines = lines, .count = count,
				.first = &first, .empty = _("There is no crash log."), .footer = crash_footer});
		if (key == INPUT_BLUE)
			socket_log = !socket_log;
		else if (key == INPUT_YELLOW && !socket_log && log_count > 1) {
			int chosen = choose_crash_log(ui, input, logs, log_count, current);
			if (chosen >= 0 && chosen != current) {
				free_lines(lines, count);
				lines = NULL;
				current = chosen;
				count = load_crash_log(logs[current].path, &lines, &first, title, sizeof(title));
			}
		} else
			break;
	}
	blue_taken = 0;
	free_lines(watch_lines, watch_count);
	free_lines(lines, count);
	free(logs);
}

static long long milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static const char *names[] = {  /* Translated where they are shown. */
	N_("Restart Enigma2"),
	N_("Show crash log"),
	N_("Send crash report"),
	N_("Disable plugins"),
	N_("Software update"),
	N_("Reset settings"),
	N_("Back up image"),
	N_("Flash online/local"),
	N_("Boot another slot")
};
static const char *icons[] = {UI_ICON_RESTART, UI_ICON_LOG, UI_ICON_REPORT, UI_ICON_PLUGINS, UI_ICON_UPDATE,
	UI_ICON_RESET, UI_ICON_BACKUP, UI_ICON_FLASH, UI_ICON_SLOT};
static const int actions[] = {ACTION_START, ACTION_CRASH_LOG, ACTION_CRASH_REPORT, ACTION_PLUGINS,
	ACTION_UPDATE, ACTION_RESET, ACTION_BACKUP, ACTION_FLASH, ACTION_SLOT};
#define ITEM_COUNT (int)(sizeof(names) / sizeof(names[0]))

static void system_info(char *text, size_t size);

/* What the chosen entry does, on the right of the menu. */
static void preview(const struct ui_context *ui, int item, const struct watch_result *result, const char *info,
	const char *footer)
{
	static const char *texts[] = {
		N_("Starts Enigma2 again. If a plugin keeps it from starting, disable the plugin first."),
		N_("Shows the newest crash log of Enigma2. BLUE shows the steps of the last start."),
		NULL,  /* With the distribution, see below. */
		N_("Keeps Enigma2 from loading a plugin, temporarily or permanently, and enables it again. A plugin that "
			"caused problems at the last start is marked and chosen at first."),
		N_("Installs the latest updates for your receiver. You see the list and confirm before anything is "
			"installed."),
		N_("Resets Enigma2 to the state of a new installation. Your old settings are moved aside, so nothing is "
			"deleted for good."),
		N_("Saves the running image as a zip file on a USB stick or hard disk, so it can be flashed again later."),
		N_("Flashes an image into the running slot, from the feed of the running distribution or from the media. "
			"A check with ofgwrite tests the image before anything is written."),
		N_("Restarts the receiver with the image of another slot.")
	};
	char text[640];
	char description[640];
	char distro[64];
	char card_title[160];
	char card[512];
	const char *extra = NULL;
	if (from_shell)
		texts[0] = N_("Ends the Open Recovery Manager. Enigma2 stays stopped until it is started again, e.g. with "
			"init 3.");
	describe(result, card_title, sizeof(card_title), card, sizeof(card));
	if (actions[item] == ACTION_START)
		extra = info;
	else if (actions[item] == ACTION_CRASH_LOG) {
		char path[512];
		if (crash_log_path(path, sizeof(path)))
			snprintf(text, sizeof(text), _("Newest crash log: %s"), strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		else
			snprintf(text, sizeof(text), "%s", _("There is no crash log."));
		extra = text;
	}
	if (actions[item] == ACTION_CRASH_REPORT) {
		boxinfo_value("displaydistro", distro, sizeof(distro));
		/* TRANSLATORS: %s is the distribution, e.g. OpenATV. */
		snprintf(description, sizeof(description), _("Sends the crash report to the %s ticket system, so the "
			"developers can find the error. You see everything before it is sent."), distro[0] ? distro : "OpenATV");
	} else
		snprintf(description, sizeof(description), "%s", _(texts[item]));
	ui_preview(ui, &(struct ui_card_page){.title = _(names[item]),
		.card_title = actions[item] == ACTION_START ? card_title : NULL,
		.card = actions[item] == ACTION_START ? card : NULL, .warn = result != NULL,
		.body = description, .info = extra, .footer = footer});
}

/* Grey and not selectable: not every image has the command line of CrashReport, not every receiver multiboot. */
static void mark_items(char *marks)
{
	for (int i = 0; i < ITEM_COUNT; ++i) {
		marks[i] = (actions[i] == ACTION_CRASH_REPORT && access(CRASHREPORT, X_OK) != 0) ||
			(actions[i] == ACTION_SLOT && !slots_multiboot()) ? 2 : 0;
		item_marks[i] = marks[i];
	}
}

/* Remote support, language and the receiver keys on the right, RED at the edge. */
static void menu_footer(char *footer, size_t size, int countdown, int left)
{
	char countdown_text[96] = "";
	char help[96] = "";
	if (countdown) {
		memset(countdown_text, ' ', 3);
		snprintf(countdown_text + 3, sizeof(countdown_text) - 3, _("Restart in %d s, any key cancels"), left);
	}
	if (languages)
		snprintf(help, sizeof(help), "HELP: %s   ", _("Language"));
	snprintf(footer, size, "OK: %s   1-%d: %s%s\t%s%s%s%sINFO: %s   YELLOW: %s   "
		"RED: %s", _("Open"), ITEM_COUNT, _("Choose"), countdown_text,
		remote_support_available() ? "BLUE: " : "", remote_support_available() ? _("Remote Support") : "",
		remote_support_available() ? "   " : "", help, _("About"), _("Reboot"), _("Shutdown"));
}

/* With a countdown until its next second, so it counts down evenly. */
static enum input_key next_key(struct input_context *input, int *selected, int countdown, long long remaining)
{
	enum input_key key;
	if (jump_to >= 0) {  /* A digit in the screen of another entry. */
		*selected = jump_to;
		jump_to = -1;
		key = INPUT_OK;
	} else
		key = input_next(input, countdown ? (int)((remaining - 1) % 1000) + 1 : 1000);
	return key;
}

/* UP and DOWN skip grey entries, RIGHT and a digit become OK. */
static int move_selection(enum input_key *key, int selected, const char *marks)
{
	if (*key == INPUT_UP) {
		do
			selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
		while (marks[selected] == 2);
	} else if (*key == INPUT_DOWN) {
		do
			selected = (selected + 1) % ITEM_COUNT;
		while (marks[selected] == 2);
	} else if (*key == INPUT_RIGHT)
		*key = INPUT_OK;
	else if ((*key >= INPUT_1 && *key < INPUT_1 + ITEM_COUNT && *key <= INPUT_9) ||
		(*key == INPUT_0 && ITEM_COUNT > 9)) {
		int chosen = *key == INPUT_0 ? 9 : *key - INPUT_1;
		*key = INPUT_NONE;
		if (marks[chosen] != 2) {
			selected = chosen;
			*key = INPUT_OK;
		}
	}
	return selected;
}

/* The screen of the entry; 1 when the receiver restarts into another slot. */
static int open_entry(struct ui_context *ui, struct input_context *input, int selected)
{
	ui_sidebar_select(ui, selected, 0);  /* The screen of the entry gets the keys. */
	in_entry = 1;
	switch (actions[selected]) {
	case ACTION_CRASH_LOG:
		show_logs(ui, input);
		break;
	case ACTION_CRASH_REPORT:
		crash_report(ui, input, &stop_requested);
		break;
	case ACTION_UPDATE:
		update_packages(ui, input, &stop_requested);
		break;
	case ACTION_SLOT:
		if (boot_slot(ui, input, &stop_requested))
			return 1;
		break;
	case ACTION_PLUGINS:
		disable_plugins(ui, input, &stop_requested);
		break;
	case ACTION_RESET:
		reset_settings(ui, input, &stop_requested);
		break;
	case ACTION_BACKUP:
		image_backup(ui, input, &stop_requested);
		break;
	case ACTION_FLASH:
		flash_image(ui, input, &stop_requested);
		break;
	default:
		break;
	}
	in_entry = 0;
	return 0;
}

/* Starts enigma2 again after the countdown unless somebody presses a key. Up and down show what an entry
 * does, OK, RIGHT or its digit opens it. */
static int recovery_menu(struct ui_context *ui, struct input_context *input,
	const struct watch_result *result, int countdown)
{
	char marks[ITEM_COUNT];
	char footer[512];
	char info[512];
	long long deadline = milliseconds() + countdown * 1000LL;
	int generation = -1;
	int selected = 0;
	mark_items(marks);
	ui_sidebar(ui, names, icons, ITEM_COUNT, marks);
	while (!stop_requested) {
		long long remaining = deadline - milliseconds();
		int left = (int)((remaining + 999) / 1000);  /* Whole seconds, each shown for a full second. */
		enum input_key key;
		if (global_pressed)
			countdown = 0;
		if (countdown && remaining <= 0)
			return ACTION_START;
		if (generation != i18n_generation()) {  /* Again in another language. */
			generation = i18n_generation();
			system_info(info, sizeof(info));
		}
		menu_footer(footer, sizeof(footer), countdown, left);
		ui_sidebar_select(ui, selected, 1);
		preview(ui, selected, result, info, footer);
		key = next_key(input, &selected, countdown, remaining);
		if (key == INPUT_NONE)
			continue;
		countdown = 0;
		selected = move_selection(&key, selected, marks);
		if (key == INPUT_RED)
			return ACTION_POWER_OFF;
		if (key == INPUT_YELLOW)
			return ACTION_REBOOT;
		if (key != INPUT_OK)
			continue;
		if (actions[selected] == ACTION_START)
			return ACTION_START;
		if (open_entry(ui, input, selected))
			return ACTION_REBOOT;
	}
	return ACTION_START;
}

/* The value of key=value or key='value' in text that starts with a newline. */
static void info_value(const char *text, const char *key, char *value, size_t size)
{
	char pattern[64];
	const char *found;
	size_t length;
	snprintf(pattern, sizeof(pattern), "\n%s=", key);
	value[0] = '\0';
	if (!(found = strstr(text, pattern)))
		return;
	found += strlen(pattern);
	if (*found == '\'')
		found++;
	length = strcspn(found, "'\n");
	snprintf(value, size, "%.*s", (int)length, found);
}

/* Like getE2Rev(): "35564+4099495" of the version "8.0.0-beta+git35564+40994950+4099495853-r1". */
static void enigma2_revision(char *revision, size_t size)
{
	FILE *file = fopen("/var/lib/opkg/status", "r");
	char line[256];
	int found = 0;
	revision[0] = '\0';
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		if (strncmp(line, "Package: ", 9) == 0)
			found = strcmp(line, "Package: enigma2\n") == 0;
		else if (found && strncmp(line, "Version: ", 9) == 0) {
			char *sha = strrchr(line, '+');
			if (sha)
				snprintf(revision, size, "%.*s", (int)strcspn(sha + 1, "-\n"), sha + 1);
			break;
		}
	}
	fclose(file);
}

/* The receiver and its image, at the top of the menu. */
static void system_info(char *text, size_t size)
{
	char info[4096] = "\n";
	char brand[64];
	char model[64];
	char machine[64];
	char distro[64];
	char version[64];
	char build[32];
	char type[32];
	char revision[64];
	FILE *file = fopen("/usr/lib/enigma.info", "r");
	if (file) {
		size_t got = fread(info + 1, 1, sizeof(info) - 2, file);
		if (got > sizeof(info) - 2)
			got = sizeof(info) - 2;
		info[got + 1] = '\0';
		fclose(file);
	}
	info_value(info, "displaybrand", brand, sizeof(brand));
	info_value(info, "displaymodel", model, sizeof(model));
	info_value(info, "machinebuild", machine, sizeof(machine));
	info_value(info, "displaydistro", distro, sizeof(distro));
	info_value(info, "imageversion", version, sizeof(version));
	info_value(info, "imagebuild", build, sizeof(build));
	info_value(info, "imagetype", type, sizeof(type));
	enigma2_revision(revision, sizeof(revision));
	snprintf(text, size, _("%s %s (%s)\n%s %s, build %s (%s)\nEnigma2 revision %s"),
		brand, model, machine, distro, version, build, type, revision[0] ? revision : _("unknown"));
}

static struct ui_context screen;  /* Global for the clock and the keys of input_next. */
static struct input_context input;

static void clock_tick(void)
{
	ui_clock(&screen);
}

static int jump_pending(void)
{
	return jump_to >= 0;
}

/* HELP, BLUE and INFO over every screen, BACK returns to it. A digit in the screen of an entry or of
 * these keys opens another entry when OK is offered there, nothing runs then. */
static int global_key(enum input_key key)
{
	static int busy;
	int focused;
	if ((in_entry || busy) && ui_offers_ok() && ((key >= INPUT_1 && key <= INPUT_9) || key == INPUT_0)) {
		int chosen = key == INPUT_0 ? 9 : key - INPUT_1;
		if (chosen < ITEM_COUNT && item_marks[chosen] != 2)
			jump_to = chosen;
		return 1;
	}
	if (busy || (key != INPUT_HELP && key != INPUT_BLUE && key != INPUT_INFO) ||
		(key == INPUT_BLUE && (!remote_support_available() || blue_taken)) || (key == INPUT_HELP && !languages))
		return 0;
	busy = 1;
	global_pressed = 1;
	focused = ui_sidebar_focus(&screen, 0);
	if (key == INPUT_BLUE)
		remote_support(&screen, &input, &stop_requested);
	else if (key == INPUT_HELP)
		language_choose(&screen, &input, &stop_requested);
	else
		about(&screen, &input, &stop_requested);
	ui_sidebar_focus(&screen, focused);
	busy = 0;
	return 1;
}

static void key_pressed(enum input_key key)
{
	ui_key_pressed(input_key_name(key));
}

static int run_menu(const struct watch_result *watch, int countdown)
{
	struct sigaction action;
	int result;
	i18n_init();
	languages = language_other();
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
	if (!ui_open(&screen)) {
		perror("framebuffer");
		return ACTION_START;
	}
	memset(&input, 0, sizeof(input));
	input_open(&input);
	input_set_clock(clock_tick);
	input_set_global(global_key);
	input_set_jump(jump_pending);
	input_set_press(key_pressed);
	ui_set_remote(remote_support_counts);
	result = recovery_menu(&screen, &input, watch, countdown);
	input_close(&input);
	ui_clear(&screen, (struct ui_color){0, 0, 0, 255});
	ui_present(&screen);
	ui_close(&screen);
	return result;
}

/* The parent is an interactive shell: sh, ash, bash or dash, also as login shell -sh, without arguments. A start
 * script, like sh enigma2.sh or sh -c, acts on the exit code itself. */
static int started_from_shell(void)
{
	static const char *const shells[] = {"sh", "ash", "bash", "dash"};
	char path[32];
	char cmdline[256] = "";  /* Zeroed, so the read stays terminated. */
	const char *name;
	size_t length;
	FILE *file;
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)getppid());
	if (!(file = fopen(path, "r")))
		return 0;
	length = fread(cmdline, 1, sizeof(cmdline) - 1, file);
	fclose(file);
	if (!length || length > strlen(cmdline) + 1)
		return 0;
	name = strrchr(cmdline, '/');
	name = name ? name + 1 : cmdline;
	if (*name == '-')
		name++;
	for (size_t i = 0; i < sizeof(shells) / sizeof(shells[0]); ++i)
		if (strcmp(name, shells[i]) == 0)
			return 1;
	return 0;
}

/* Powers off or reboots like enigma2.sh, for a start by hand. */
static void power(int action)
{
	FILE *file;
	if (action == ACTION_POWER_OFF)
		execl("/sbin/halt", "halt", (char *)NULL);
	else if (action == ACTION_REBOOT) {
		if ((file = fopen("/proc/stb/fp/force_restart", "w"))) {
			fputs("1", file);
			fclose(file);
		}
		execl("/sbin/reboot", "reboot", (char *)NULL);
	} else
		return;
	perror("power");
}

static void usage(FILE *out, const char *program)
{
	fprintf(out,
		"recovery-manager %s - Open Recovery Manager (ORM)\n"
		"Steps in when Enigma2 does not start, e.g. after a broken update or plugin: shows where the start\n"
		"stopped and offers to restart, disable plugins, update, reset, back up, flash and Remote Support.\n"
		"\n"
		"Usage: %s --manual              show the recovery menu on request\n"
		"       %s --crash RESULT [PID]  show the recovery menu after a failed start, PID ends the watch\n"
		"       %s --watch RESULT        watch the start of enigma2 (from enigma2.sh)\n"
		"       %s --version\n"
		"       %s --help\n"
		"\n"
		"The menu needs the screen: stop Enigma2 with init 4 first and start it again with init 3.\n"
		"RESULT holds the lines failed=, reason=, ready=, uptime=, step= and crash=, e.g.\n"
		"  printf 'failed=1\\nreason=crash\\nready=0\\nuptime=0\\nstep=Plugin AutoTimer\\ncrash=11\\n'"
		" > /tmp/orm.result\n",
		orm_version(), program, program, program, program, program);
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--watch") == 0)
		return watch_run(argv[2]);
	if ((argc == 3 || argc == 4) && strcmp(argv[1], "--crash") == 0) {
		struct watch_result result;
		if (argc == 4)  /* From enigma2.sh after enigma2 ended: the menu only after a failed start. */
			watch_stop((pid_t)atoi(argv[3]));
		if (!watch_read_result(argv[2], &result)) {
			fprintf(stderr, "%s cannot be read\n", argv[2]);
			return ACTION_START;
		}
		if (argc == 4 && !result.failed)
			return ACTION_START;
		return run_menu(&result, COUNTDOWN_SECONDS);
	}
	if (argc == 2 && strcmp(argv[1], "--manual") == 0) {
		int action;
		if ((from_shell = started_from_shell())) {
			names[0] = N_("Exit");
			icons[0] = UI_ICON_EXIT;
		}
		action = run_menu(NULL, 0);
		if (from_shell)
			power(action);
		return action;
	}
	if (argc == 2 && strcmp(argv[1], "--version") == 0) {
		printf("recovery-manager %s\n", orm_version());
		return 0;
	}
	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		usage(stdout, argv[0]);
		return 0;
	}
	usage(stderr, argv[0]);
	return 2;
}

#define _GNU_SOURCE

#include "repair.h"

#include "boxinfo.h"
#include "i18n.h"
#include "process.h"
#include "viewer.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TITLE _("Enigma2 system files")
#define PYTHON "/usr/lib/enigma2/python"
#define PACKAGE_LISTS "/var/lib/opkg/info"
#define PACKAGE_STATUS "/var/lib/opkg/status"
#define ASIDE "/home/root/enigma2-repair-%s"  /* With the time, like the reset of the settings. */
#define MAX_FILES 65536

struct owned {
	char **paths;
	int count;
	int size;
};

static int by_path(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int ends_with(const char *text, const char *end)
{
	size_t a = strlen(text);
	size_t b = strlen(end);
	return a >= b && !strcmp(text + a - b, end);
}

static int is_python(const char *name)
{
	return ends_with(name, ".py") || ends_with(name, ".pyc") || ends_with(name, ".so");
}

static void push(char ***paths, int *count, int *size, const char *path)
{
	char *copy;
	if (*count >= MAX_FILES)
		return;
	if (*count >= *size) {
		int grown = *size ? *size * 2 : 256;
		char **more = realloc(*paths, (size_t)grown * sizeof(*more));
		if (!more)
			return;
		*paths = more;
		*size = grown;
	}
	if ((copy = strdup(path)))
		(*paths)[(*count)++] = copy;
}

/* The files below PYTHON of every installed package, "path<TAB>mode" in its list; those of enigma2 also in
 * mine. */
static void read_owned(struct owned *o, struct owned *mine)
{
	DIR *dir = opendir(PACKAGE_LISTS);
	const struct dirent *entry;
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char path[320];
		char line[1024];
		FILE *file;
		if (!ends_with(entry->d_name, ".list"))
			continue;
		snprintf(path, sizeof(path), PACKAGE_LISTS "/%.255s", entry->d_name);
		if (!(file = fopen(path, "r")))
			continue;
		while (fgets(line, sizeof(line), file)) {
			text_cut(line, "\t\r\n");
			if (strncmp(line, PYTHON "/", strlen(PYTHON) + 1))
				continue;
			push(&o->paths, &o->count, &o->size, line);
			if (!strcmp(entry->d_name, "enigma2.list") && is_python(line))
				push(&mine->paths, &mine->count, &mine->size, line);
		}
		fclose(file);
	}
	closedir(dir);
	if (o->count)
		qsort(o->paths, (size_t)o->count, sizeof(o->paths[0]), by_path);
}

/* When enigma2 was installed, 0 when unknown. */
static time_t installed_time(void)
{
	FILE *file = fopen(PACKAGE_STATUS, "r");
	char *line = NULL;
	size_t size = 0;
	int block = 0;
	time_t installed = 0;
	if (!file)
		return 0;
	while (getline(&line, &size, file) > 0) {
		text_cut(line, "\r\n");
		if (!line[0])
			block = 0;
		else if (!strncmp(line, "Package: ", 9))
			block = !strcmp(line + 9, "enigma2");
		else if (block && !strncmp(line, "Installed-Time: ", 16))
			installed = (time_t)atoll(line + 16);
	}
	free(line);
	fclose(file);
	return installed;
}

/* The files of enigma2 written after it was installed; a package keeps the time of its build. */
static void find_changed(struct foreign *f, const struct owned *mine)
{
	time_t installed = installed_time();
	int size = 0;
	if (!installed)
		return;
	for (int i = 0; i < mine->count; ++i) {
		struct stat info;
		if (stat(mine->paths[i], &info) == 0 && info.st_mtime > installed + 60)  /* Not the install itself. */
			push(&f->changed, &f->changed_count, &size, mine->paths[i]);
	}
}

static int is_owned(const struct owned *o, const char *path)
{
	return o->count && bsearch(&path, o->paths, (size_t)o->count, sizeof(o->paths[0]), by_path);
}

/* The python files of folder, below it too up to depth, that no package has. */
static void scan(struct foreign *f, int *size, const struct owned *o, const char *folder, int depth)
{
	DIR *dir = opendir(folder);
	const struct dirent *entry;
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char path[512];
		struct stat info;
		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%.300s/%.200s", folder, entry->d_name);
		if (lstat(path, &info) != 0)
			continue;
		if (S_ISDIR(info.st_mode)) {
			if (depth > 0)
				scan(f, size, o, path, depth - 1);
		} else if (S_ISREG(info.st_mode) && is_python(entry->d_name) && !is_owned(o, path))
			push(&f->paths, &f->count, size, path);
	}
	closedir(dir);
}

void repair_find_foreign(struct foreign *f)
{
	static const char *const folders[] = {PYTHON "/Components", PYTHON "/Screens", PYTHON "/Tools"};
	struct owned o = {0};
	struct owned mine = {0};
	int size = 0;
	memset(f, 0, sizeof(*f));
	read_owned(&o, &mine);
	if (o.count) {  /* Without the package lists everything would look foreign. */
		scan(f, &size, &o, PYTHON, 0);  /* Plugins has its own packages, disabling them is the rest of the screen. */
		for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); ++i)
			scan(f, &size, &o, folders[i], 8);
		if (f->count)
			qsort(f->paths, (size_t)f->count, sizeof(f->paths[0]), by_path);
		find_changed(f, &mine);
	}
	for (int i = 0; i < o.count; ++i)
		free(o.paths[i]);
	free(o.paths);
	for (int i = 0; i < mine.count; ++i)
		free(mine.paths[i]);
	free(mine.paths);
}

int repair_is_foreign(const struct foreign *f, const char *path)
{
	char compiled[520];
	const char *key = compiled;
	if (f->count && bsearch(&path, f->paths, (size_t)f->count, sizeof(f->paths[0]), by_path))
		return 1;
	snprintf(compiled, sizeof(compiled), "%sc", path);  /* The traceback names the .py of a .pyc. */
	for (int i = 0; i < f->changed_count; ++i)
		if (!strcmp(f->changed[i], path) || !strcmp(f->changed[i], key))
			return 2;
	return 0;
}

void repair_free_foreign(struct foreign *f)
{
	for (int i = 0; i < f->count; ++i)
		free(f->paths[i]);
	free(f->paths);
	for (int i = 0; i < f->changed_count; ++i)
		free(f->changed[i]);
	free(f->changed);
	memset(f, 0, sizeof(*f));
}

static void make_folders(const char *path)  /* mkdir -p of the folder of path */
{
	char folder[768];
	char *slash;
	snprintf(folder, sizeof(folder), "%s", path);
	slash = strchr(folder + 1, '/');
	while (slash) {
		*slash = '\0';
		mkdir(folder, 0755);
		*slash = '/';
		slash = strchr(slash + 1, '/');
	}
}

/* Every foreign file to the same path below aside, kept there in case an add-on needs it after all. */
static int move_aside(const struct foreign *f, const char *aside, char *error, size_t size)
{
	for (int i = 0; i < f->count; ++i) {
		char to[768];
		snprintf(to, sizeof(to), "%s%s", aside, f->paths[i]);
		make_folders(to);
		if (rename(f->paths[i], to) != 0) {  /* NOSONAR moving the foreign file aside is the repair */
			snprintf(error, size, _("%s cannot be moved: %s"), f->paths[i], strerror(errno));
			return 0;
		}
	}
	return 1;
}

/* The version in "enigma2 - 8.0.1+git... - ..." of opkg list or list-installed, empty when none. */
static void version(const char *command, char *text, size_t size)
{
	char output[1024] = "";
	char verb[16];
	char *start;
	char *const argv[] = {"opkg", verb, "enigma2", NULL};
	snprintf(verb, sizeof(verb), "%s", command);
	text[0] = '\0';
	if (process_capture(argv, output, sizeof(output)) != 0 || strncmp(output, "enigma2 - ", 10))
		return;
	start = output + 10;
	snprintf(text, size, "%.*s", (int)strcspn(start, " \r\n"), start);
}

static int ask_repair(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct foreign *f)
{
	char intro[640];
	char title[128];
	char footer[128];
	char **lines = calloc((size_t)f->count + (size_t)f->changed_count + 34, sizeof(*lines));
	char *rows[30] = {NULL};
	const char *rest;
	int first = 0;
	int count = 0;
	enum input_key key;
	if (!lines)
		return 0;
	if (f->count)
		snprintf(intro, sizeof(intro), "%s", ngettext("This file does not belong to any installed package. A skin "
			"or add-on put it into the folders of Enigma2, where it can replace a file of Enigma2. The repair moves it "
			"aside and installs Enigma2 again from the feed, you are asked before.", "These files do not belong to any "
			"installed package. A skin or add-on put them into the folders of Enigma2, where they can replace files of "
			"Enigma2. The repair moves them aside and installs Enigma2 again from the feed, you are asked before.",
			f->count));
	else if (f->changed_count)
		snprintf(intro, sizeof(intro), "%s", ngettext("This file of Enigma2 was changed after it was installed, e.g. "
			"by a file of a skin or add-on. The repair installs Enigma2 again from the feed, which restores it, you "
			"are asked before.", "These files of Enigma2 were changed after it was installed, e.g. by a file of a "
			"skin or add-on. The repair installs Enigma2 again from the feed, which restores them, you are asked "
			"before.", f->changed_count));
	else
		snprintf(intro, sizeof(intro), "%s", _("No foreign files were found. The repair installs Enigma2 again from "
			"the feed, which restores its original files, you are asked before."));
	rest = intro;
	while (*rest && count < 30) {  /* Broken like the crash log. */
		size_t skip;
		size_t length = text_break(rest, &skip);
		rows[count] = strndup(rest, length);
		lines[count] = rows[count] ? rows[count] : "";
		rest += length + skip;
		count++;
	}
	if (f->count)
		lines[count++] = "";
	for (int i = 0; i < f->count; ++i)
		lines[count++] = f->paths[i];
	if (f->count && f->changed_count) {
		lines[count++] = "";
		lines[count++] = _("Changed files of Enigma2, installing it again restores them:");
	} else if (f->changed_count)
		lines[count++] = "";
	for (int i = 0; i < f->changed_count; ++i)
		lines[count++] = f->changed[i];
	if (f->count)
		snprintf(title, sizeof(title), ngettext("%s, %d foreign file", "%s, %d foreign files", f->count), TITLE,
			f->count);
	else if (f->changed_count)
		snprintf(title, sizeof(title), ngettext("%s, %d changed file", "%s, %d changed files", f->changed_count),
			TITLE, f->changed_count);
	else
		snprintf(title, sizeof(title), "%s", TITLE);
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Scroll"), .ok = _("Repair"),
		.back = _("Back")});
	do
		key = text_view(ui, input, stop, &(struct text_page){.title = title, .lines = lines, .count = count,
			.first = &first, .empty = "", .footer = footer});
	while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_NONE);
	for (int i = 0; i < 30; ++i)
		free(rows[i]);
	free(lines);
	return key == INPUT_OK;
}

/* Asked every time and before anything changes: without installing Enigma2 again, files compiled from the
 * foreign ones stay. */
static void question_text(const struct foreign *f, char *question, size_t size)
{
	char installed[96];
	char feed[96];
	version("list-installed", installed, sizeof(installed));
	version("list", feed, sizeof(feed));
	if (installed[0] && feed[0] && strcmp(installed, feed))
		snprintf(question, size, _("The feed has Enigma2 %s, installed is %s. The repair installs it without the "
			"other packages, the software update updates all of them. Repair anyway?"), feed, installed);
	else
		snprintf(question, size, _("Install Enigma2 %s again from the feed now?"), installed[0] ? installed : feed);
	if (f->count)
		snprintf(question + strlen(question), size - strlen(question), " %s",
			ngettext("The foreign file is moved aside first.", "The foreign files are moved aside first.", f->count));
}

void repair_system_files(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct foreign *f)
{
	struct live_output output = {.ui = ui};
	char aside[64] = "";
	char note[512];
	char question[640];
	char title[128];
	int result;
	if (!ask_repair(ui, input, stop, f))
		return;
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Updating the package lists"));
	output.title = title;
	output.footer = _("Please wait...");
	{
		char *const update[] = {"opkg", "update", NULL};
		result = process_run_with_updates(update, NULL, live_output_line, live_output_tick, 100, &output);
	}
	for (int i = 0; result != 0 && i < output.count; ++i)  /* Like the software update. */
		if (strstr(output.lines[i], "Updated source") && strstr(output.lines[i], "openatv-all"))
			result = 0;
	if (result != 0) {
		live_output_view(&output, input, stop, _("The package lists could not be updated, nothing was changed. The "
			"repair needs the feed."), 0);
		live_output_free(&output);
		return;
	}
	question_text(f, question, sizeof(question));
	if (!ask(ui, input, stop, TITLE, question, 0)) {
		live_output_free(&output);
		return;
	}
	if (f->count) {
		char stamp[32];
		char error[320];
		struct tm local;
		time_t now = time(NULL);
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime_r(&now, &local));
		snprintf(aside, sizeof(aside), ASIDE, stamp);
		if (!move_aside(f, aside, error, sizeof(error))) {
			live_output_free(&output);
			ui_error(ui, TITLE, error);
			while (input_next(input, 1000) != INPUT_OK && !(stop && *stop))
				;
			return;
		}
	}
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Installing Enigma2 again"));
	output.footer = _("Please wait, do not switch off the receiver...");
	{
		char *const install[] = {"opkg", "install", "--force-reinstall", "--force-downgrade", "enigma2", NULL};
		result = process_run_with_updates(install, NULL, live_output_line, live_output_tick, 100, &output);
	}
	if (result == 0 && aside[0])
		snprintf(note, sizeof(note), _("Enigma2 is installed again, the foreign files are in %s. Restart Enigma2 "
			"in the menu."), aside);
	else if (result == 0)
		snprintf(note, sizeof(note), "%s", _("Enigma2 is installed again. Restart Enigma2 in the menu."));
	else
		snprintf(note, sizeof(note), "%s", _("Enigma2 could not be installed again."));
	live_output_view(&output, input, stop, note, result == 0);
	live_output_free(&output);
}

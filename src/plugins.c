#define _GNU_SOURCE

#include "plugins.h"

#include "boxinfo.h"
#include "crash.h"
#include "i18n.h"
#include "viewer.h"
#include "watch.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define TITLE _("Disable plugins")
#define PYTHON "/usr/lib/enigma2/python"
#define MAX_ITEMS 512
#define BLACKLIST "/etc/enigma2/plugin_blacklist"
#define BLACKLIST_TEMPORARY "/tmp/plugin_blacklist"

enum state {
	ENABLED,
	MOVED,  /* In Plugins.disabled. */
	TEMPORARY,  /* In BLACKLIST_TEMPORARY. */
	BLACKLISTED  /* In BLACKLIST. */
};

/* Only plugins: a broken screen, tool, converter or renderer of enigma2 is not fixed by disabling it. */
struct kind {
	const char *folder;  /* Below PYTHON. */
	const char *disabled;  /* Below DISABLED_ROOT. */
	const char *type;  /* Translated where it is shown. */
};

static const struct kind kinds[] = {
	{"Plugins/Extensions", "Extensions", N_("Plugin")},
	{"Plugins/SystemPlugins", "SystemPlugins", N_("System plugin")},
};

struct item {
	int kind;
	char name[64];
	enum state state;
	char problem[160];
};

struct list {
	struct item items[MAX_ITEMS];
	int count;
	int blacklist;  /* Enigma2 knows the blacklists. */
	const char *system_cause;  /* The module of enigma2 that was missing, no plugin is to blame. */
};

static int is_directory(const char *path)
{
	struct stat info;
	return lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

/* The whole file, NULL when it cannot be read or is empty or larger than 16 MB. */
static char *read_data(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");  /* NOSONAR a file of enigma2 or of an installed plugin */
	struct stat info;
	char *data = NULL;
	*size = 0;
	if (!file)
		return NULL;
	if (fstat(fileno(file), &info) == 0 && info.st_size > 0 && info.st_size <= 16 * 1024 * 1024)
		data = malloc((size_t)info.st_size);
	if (data && fread(data, 1, (size_t)info.st_size, file) == (size_t)info.st_size)
		*size = (size_t)info.st_size;
	else {
		free(data);
		data = NULL;
	}
	fclose(file);
	return data;
}

static int file_has(const char *path, const char *text)
{
	size_t size;
	char *data = read_data(path, &size);
	int found = data && memmem(data, size, text, strlen(text));
	free(data);
	return found;
}

/* Enigma2 reads the blacklists: its Tools/Directories has their file name, also as .pyc. */
static int blacklist_supported(void)
{
	DIR *dir;
	const struct dirent *entry;
	int found = 0;
	if (file_has(PYTHON "/Tools/Directories.pyc", "plugin_blacklist"))
		return 1;
	if (file_has(PYTHON "/Tools/Directories.py", "plugin_blacklist"))
		return 1;
	dir = opendir(PYTHON "/Tools/__pycache__");
	if (!dir)
		return 0;
	while (!found) {
		char path[320];
		entry = readdir(dir);
		if (!entry)
			break;
		if (strncmp(entry->d_name, "Directories.", 12))
			continue;
		snprintf(path, sizeof(path), PYTHON "/Tools/__pycache__/%.200s", entry->d_name);
		found = file_has(path, "plugin_blacklist");
	}
	closedir(dir);
	return found;
}

/* The name in line without the white space around it, like enigma2 reads it; plugin names have none
 * inside. 0 for an empty line. */
static int entry_name(const char *line, char *name)
{
	return sscanf(line, "%255s", name) == 1;
}

static int in_blacklist(const char *path, const char *name)
{
	FILE *file = fopen(path, "r");
	char line[256];
	char entry[256];
	int found = 0;
	if (!file)
		return 0;
	while (!found && fgets(line, sizeof(line), file))
		if (entry_name(line, entry))
			found = !strcmp(entry, name);
	fclose(file);
	return found;
}

/* Adds name to the blacklist path or removes it; the file is removed when it gets empty. */
static int blacklist_set(const char *path, const char *name, int add, char *error, size_t size)
{
	char temporary[64];
	char line[256];
	char entry[256];
	FILE *in = fopen(path, "r");
	FILE *out;
	int lines = 0;
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	if (!(out = fopen(temporary, "w"))) {
		snprintf(error, size, _("%s cannot be written: %s"), path, strerror(errno));
		if (in)
			fclose(in);
		return 0;
	}
	while (in && fgets(line, sizeof(line), in)) {
		if (entry_name(line, entry) && strcmp(entry, name)) {
			fprintf(out, "%s\n", entry);
			lines++;
		}
	}
	if (in)
		fclose(in);
	if (add) {
		fprintf(out, "%s\n", name);
		lines++;
	}
	if (fclose(out) == 0) {
		if (lines && rename(temporary, path) == 0)
			return 1;
		if (!lines && (unlink(path) == 0 || errno == ENOENT)) {
			unlink(temporary);
			return 1;
		}
	}
	snprintf(error, size, _("%s cannot be written: %s"), path, strerror(errno));
	unlink(temporary);
	return 0;
}

/* The plugins in the blacklists; enigma2 matches the folder name only, not the type. */
static void apply_blacklists(struct list *l)
{
	for (int i = 0; i < l->count; ++i) {
		struct item *item = &l->items[i];
		if (item->state != ENABLED)
			continue;
		if (in_blacklist(BLACKLIST, item->name))
			item->state = BLACKLISTED;
		else if (in_blacklist(BLACKLIST_TEMPORARY, item->name))
			item->state = TEMPORARY;
	}
}

static int find(const struct list *l, int kind, const char *name)
{
	for (int i = 0; i < l->count; ++i)
		if ((kind < 0 || l->items[i].kind == kind) && !strcmp(l->items[i].name, name))
			return i;
	return -1;
}

static void add(struct list *l, int kind, const char *name, int disabled)
{
	if (l->count == MAX_ITEMS || find(l, kind, name) >= 0)
		return;
	l->items[l->count].kind = kind;
	snprintf(l->items[l->count].name, sizeof(l->items[0].name), "%.63s", name);
	l->items[l->count].state = disabled ? MOVED : ENABLED;
	l->items[l->count].problem[0] = '\0';
	l->count++;
}

static void scan(struct list *l, int kind, int disabled)
{
	const struct kind *k = &kinds[kind];
	char folder[192];
	DIR *dir;
	struct dirent *entry;
	if (disabled)
		snprintf(folder, sizeof(folder), DISABLED_ROOT "/%s", k->disabled);
	else
		snprintf(folder, sizeof(folder), PYTHON "/%s", k->folder);
	dir = opendir(folder);
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char path[256];
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__pycache__") || strlen(entry->d_name) >= 64)
			continue;
		snprintf(path, sizeof(path), "%s/%.63s", folder, entry->d_name);
		if (is_directory(path))
			add(l, kind, entry->d_name, disabled);
	}
	closedir(dir);
}

static void set_problem(struct list *l, int kind, const char *name, const char *problem)
{
	int i = find(l, kind, name);
	if (i >= 0 && !l->items[i].problem[0])
		snprintf(l->items[i].problem, sizeof(l->items[0].problem), "%.159s", problem);
}

/* The last start step on the socket: enigma2 stopped while loading a plugin. */
static void start_problem(struct list *l)
{
	struct watch_result result;
	char text[160];
	const char *name;
	if (!watch_read_result(WATCH_RESULT, &result) || !result.failed || strncmp(result.step, "Plugin ", 7))
		return;
	if (!strcmp(result.reason, "hang"))
		snprintf(text, sizeof(text), "%s", _("Stopped responding while loading"));
	else if (!strcmp(result.crash, "python"))
		snprintf(text, sizeof(text), "%s", _("Python error while loading"));
	else if ((name = watch_signal_name(result.crash)))
		snprintf(text, sizeof(text), _("Crashed while loading (%s)"), name);
	else
		snprintf(text, sizeof(text), "%s", _("Crashed while loading"));
	set_problem(l, -1, result.step + 7, text);
}

/* "No module named 'Plugins.Extensions.Foo'" of a plugin that is disabled here, -1 when not. */
static int missing_disabled(const struct list *l, const char *error)
{
	char module[128];
	const char *start = strstr(error, "No module named '");
	if (!start)
		return -1;
	start += 17;
	snprintf(module, sizeof(module), "%.*s", (int)strcspn(start, "'"), start);
	for (char *c = module; *c; ++c)
		if (*c == '.')
			*c = '/';
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
		size_t length = strlen(kinds[k].folder);
		char name[64];
		int i;
		if (strncmp(module, kinds[k].folder, length) || module[length] != '/')
			continue;
		snprintf(name, sizeof(name), "%.*s", (int)strcspn(module + length + 1, "/"), module + length + 1);
		i = find(l, (int)k, name);
		if (i >= 0 && l->items[i].state == MOVED)
			return i;
	}
	return -1;
}

/* The text in the parentheses after end, NULL when there are none. */
static char *log_error(char *end)
{
	char *error = strchr(end + 1, '(');
	char *close;
	if (!error)
		return NULL;
	error++;
	text_cut(error, "\r\n");
	close = strrchr(error, ')');
	if (close && !close[1])  /* Only the last one. */
		*close = '\0';
	return error;
}

/* problem for the plugin folder/name, "Needed by" for the disabled item needed. */
static void set_plugin_problem(struct list *l, const char *folder, const char *name, const char *problem, int needed)
{
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
		char by[96];
		if (strcmp(kinds[k].disabled, folder))
			continue;
		set_problem(l, (int)k, name, problem);
		snprintf(by, sizeof(by), _("Needed by %.63s"), name);
		if (needed >= 0)
			set_problem(l, l->items[needed].kind, l->items[needed].name, by);
	}
}

/* "[PluginComponent] Error: Plugin 'Extensions/Foo' failed to load!  (error)" in the debug log. */
static void debug_log_problems(struct list *l)
{
	char path[512];
	char line[1024];
	FILE *file;
	if (!debug_log_path(path, sizeof(path)))
		return;
	file = fopen(path, "r");  /* NOSONAR the debug log of enigma2 */
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		char *start = strstr(line, "[PluginComponent] Error: Plugin '");
		char *slash;
		char *end;
		const char *error;
		char problem[160];
		int needed;
		if (!start)
			continue;
		start += 33;
		slash = strchr(start, '/');
		end = slash ? strchr(slash, '\'') : NULL;
		if (!end || !strstr(end, "failed to load"))
			continue;
		*slash = '\0';
		*end = '\0';
		error = log_error(end);
		needed = error ? missing_disabled(l, error) : -1;
		if (needed >= 0)  /* Disabled here, so enabling it helps. */
			snprintf(problem, sizeof(problem), _("Needs %s (%s), which is disabled"),
				l->items[needed].name, _(kinds[l->items[needed].kind].type));
		else
			snprintf(problem, sizeof(problem), _("Failed to load: %.140s"), error ? error : _("unknown error"));
		set_plugin_problem(l, start, slash + 1, problem, needed);
	}
	fclose(file);
}

/* Python files of plugins in the traceback of the newest crash log. */
static void crash_log_problems(struct list *l)
{
	char path[512];
	char line[1024];
	FILE *file;
	if (!crash_log_path(path, sizeof(path)))
		return;
	file = fopen(path, "r");  /* NOSONAR the crash log of enigma2 */
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		char *start = strstr(line, "File \"" PYTHON "/");
		if (!start)
			continue;
		start += strlen("File \"" PYTHON "/");
		for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
			size_t length = strlen(kinds[k].folder);
			char name[64];
			if (strncmp(start, kinds[k].folder, length) || start[length] != '/')
				continue;
			snprintf(name, sizeof(name), "%.*s", (int)strcspn(start + length + 1, "/\""), start + length + 1);
			set_problem(l, (int)k, name, _("In the traceback of the crash"));
		}
	}
	fclose(file);
}

static int by_kind_and_name(const void *a, const void *b)
{
	const struct item *x = a;
	const struct item *y = b;
	if (x->kind != y->kind)
		return x->kind - y->kind;
	return strcasecmp(x->name, y->name);
}

/* A rename between the folder of enigma2 and Plugins.disabled on the same file system. */
static int move(const char *from, const char *to, char *error, size_t size)
{
	struct stat info;
	if (lstat(to, &info) == 0) {  /* An update installed it again meanwhile. */
		snprintf(error, size, _("%s is there twice, remove one of them first."), to);
		return 0;
	}
	if (rename(from, to) != 0) {  /* NOSONAR moving a plugin folder is the purpose */
		snprintf(error, size, _("%s cannot be moved: %s"), from, strerror(errno));
		return 0;
	}
	return 1;
}

static void make_folders(const char *path)  /* mkdir -p */
{
	char folder[256];
	char *slash;
	snprintf(folder, sizeof(folder), "%s", path);
	slash = strchr(folder + 1, '/');
	while (slash) {
		*slash = '\0';
		mkdir(folder, 0755);
		*slash = '/';
		slash = strchr(slash + 1, '/');
	}
	mkdir(folder, 0755);
}

/* Moves item to Plugins.disabled or back. */
static int toggle(const struct item *item, char *error, size_t size)
{
	const struct kind *k = &kinds[item->kind];
	char enabled[192];
	char disabled[192];
	char from[256];
	char to[256];
	int back = item->state == MOVED;
	snprintf(enabled, sizeof(enabled), PYTHON "/%s", k->folder);
	snprintf(disabled, sizeof(disabled), DISABLED_ROOT "/%s", k->disabled);
	make_folders(back ? enabled : disabled);
	snprintf(from, sizeof(from), "%s/%s", back ? disabled : enabled, item->name);
	snprintf(to, sizeof(to), "%s/%s", back ? enabled : disabled, item->name);
	return move(from, to, error, size);
}

/* What a plugin is known by in other files: its module name. */
struct needle {
	char module[128];
};

static int identifier_char(int c)
{
	return isalnum(c) || c == '_' || c == '.';
}

/* A .pyc has the length byte in front of a name, the letter after it may be the next type. */
static int has_module(const char *data, size_t size, const char *module)
{
	size_t length = strlen(module);
	const char *end = data + size;
	const char *found = memmem(data, size, module, length);
	while (found) {
		int length_byte = found > data && (unsigned char)found[-1] == length;
		int before = found == data || length_byte || !identifier_char((unsigned char)found[-1]);
		int after = found + length == end || found[length] == '.' || length_byte ||
			!identifier_char((unsigned char)found[length]);
		if (before && after)
			return 1;
		++found;
		found = memmem(found, (size_t)(end - found), module, length);
	}
	return 0;
}

static int file_uses(const char *path, const struct needle *n)
{
	size_t size;
	char *data = read_data(path, &size);
	int used = data && has_module(data, size, n->module);
	free(data);
	return used;
}

static int ends_with(const char *text, const char *end)
{
	size_t a = strlen(text);
	size_t b = strlen(end);
	return a >= b && !strcmp(text + a - b, end);
}

/* The .py and .pyc files below folder. */
static int folder_uses(const char *folder, const struct needle *n, int depth)
{
	DIR *dir = opendir(folder);  /* NOSONAR a folder of the installed plugins */
	int used = 0;
	if (!dir)
		return 0;
	while (!used) {
		const struct dirent *entry = readdir(dir);
		char path[512];
		if (!entry)
			break;
		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%.300s/%.200s", folder, entry->d_name);
		if (is_directory(path))
			used = depth < 6 && folder_uses(path, n, depth + 1);
		else if (ends_with(entry->d_name, ".py") || ends_with(entry->d_name, ".pyc"))
			used = file_uses(path, n);
	}
	closedir(dir);
	return used;
}

static int item_uses(const struct item *item, const struct needle *n)
{
	char path[256];
	snprintf(path, sizeof(path), PYTHON "/%s/%s", kinds[item->kind].folder, item->name);
	return folder_uses(path, n, 0);
}

/* The module name of item, e.g. "Plugins.Extensions.AutoTimer". */
static void make_needle(const struct item *item, struct needle *n)
{
	snprintf(n->module, sizeof(n->module), "%s.%s", kinds[item->kind].folder, item->name);
	for (char *c = n->module; *c; ++c)
		if (*c == '/')
			*c = '.';
}

/* The enabled plugins that use item index, empty when none. */
static void users(const struct list *l, int index, char *text, size_t size)
{
	struct needle n;
	size_t used = 0;
	int count = 0;
	int more = 0;
	make_needle(&l->items[index], &n);
	text[0] = '\0';
	for (int i = 0; i < l->count; ++i) {
		if (i == index || l->items[i].state != ENABLED || !item_uses(&l->items[i], &n))
			continue;
		if (count++ < 6)
			used += (size_t)snprintf(text + used, size - used, "%s%s (%s)", used ? ", " : "",
				l->items[i].name, _(kinds[l->items[i].kind].type));
		else
			more++;
		if (used >= size)
			return;
	}
	if (more && used < size)
		snprintf(text + used, size - used, ngettext(" and %d more", " and %d more", more), more);
}

static enum input_key wait_ok(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	while ((key = input_next(input, 1000)) != INPUT_OK && key != INPUT_BACK && !(stop && *stop))
		;
	return key;
}

/* Scans the add-ons and their problems, sorted by kind and name. */
static void load_list(struct list *l)
{
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
		scan(l, (int)k, 0);
		scan(l, (int)k, 1);
	}
	if ((l->blacklist = blacklist_supported()))
		apply_blacklists(l);
	qsort(l->items, (size_t)l->count, sizeof(l->items[0]), by_kind_and_name);
	start_problem(l);
	debug_log_problems(l);
	crash_log_problems(l);
}

/* Marks the items with a problem and returns their number, selected is the first of them or 0. */
static int mark_problems(const struct list *l, char *marks, int *selected)
{
	int problems = 0;
	*selected = -1;
	for (int i = 0; i < l->count; ++i)
		if (l->items[i].problem[0]) {
			marks[i] = 1;
			problems++;
			if (*selected < 0)
				*selected = i;  /* The first one with a problem is chosen at first. */
		}
	if (*selected < 0)
		*selected = 0;
	return problems;
}

/* What OK does next, for the footer. With the blacklists a plugin goes enabled, temporarily disabled,
 * disabled and enabled again, without them it is moved to Plugins.disabled and back. */
static const char *next_action(const struct list *l, const struct item *item)
{
	if (item->state == TEMPORARY)
		return _("Disable permanently");
	if (item->state != ENABLED)
		return _("Enable");
	return l->blacklist ? _("Disable temporarily") : _("Disable");
}

static void show_list(const struct ui_context *ui, const struct list *l, char (*labels)[300], const char **items,
	char *marks, int problems, int selected)
{
	char footer[128];
	char body[512];
	char title[128];
	int disabled = 0;
	char header[96];
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"),
		.ok = next_action(l, &l->items[selected]), .back = _("Menu")});
	if (problems)
		snprintf(body, sizeof(body), "%s", ngettext("The plugin that caused the problem is marked. Changes take "
			"effect when Enigma2 starts again.", "The plugins that caused the problem are marked. Changes take "
			"effect when Enigma2 starts again.", problems));
	else if (l->system_cause)
		snprintf(body, sizeof(body), "%s", _("No plugin caused the problem, Enigma2 itself is affected. Disabling "
			"plugins does not help here, a software update or flashing an image can."));
	else
		snprintf(body, sizeof(body), "%s", _("Changes take effect when Enigma2 starts again."));
	if (l->blacklist)
		snprintf(body + strlen(body), sizeof(body) - strlen(body), " %s",
			_("Plugins disabled temporarily are enabled again when the receiver restarts."));
	snprintf(header, sizeof(header), "%s\t%s\t%s", _("Name"), _("Type"), _("Problem"));
	for (int i = 0; i < l->count; ++i) {
		char name[96];
		if (l->items[i].state == TEMPORARY)
			snprintf(name, sizeof(name), _("%s (temporarily)"), l->items[i].name);
		else if (l->items[i].state != ENABLED)
			snprintf(name, sizeof(name), _("%s (disabled)"), l->items[i].name);
		else
			snprintf(name, sizeof(name), "%s", l->items[i].name);
		snprintf(labels[i], sizeof(labels[i]), "%s\t%s\t%s", name, _(kinds[l->items[i].kind].type),
			l->items[i].problem);
		items[i] = labels[i];
		if (l->items[i].problem[0])
			marks[i] = 1;  /* Yellow. */
		else if (l->items[i].state != ENABLED)
			marks[i] = 2;  /* Grey. */
		else
			marks[i] = 0;
	}
	for (int i = 0; i < l->count; ++i)
		disabled += l->items[i].state != ENABLED;
	if (disabled)
		snprintf(title, sizeof(title), ngettext("%s (%d disabled)", "%s (%d disabled)", disabled), TITLE, disabled);
	else
		snprintf(title, sizeof(title), "%s", TITLE);
	ui_menu_table(ui, &(struct ui_menu){.title = title, .body = body, .header = header, .items = items,
		.count = l->count, .selected = selected, .marks = marks, .marked = -1, .footer = footer});
}

/* 1 when nothing enabled uses item index or the user disables it anyway. */
static int confirm_disable(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct list *l, int index)
{
	char by[480];
	char text[640];
	char keys[96];
	ui_screen(ui, TITLE, _("Checking what uses it..."), " ");
	users(l, index, by, sizeof(by));
	if (!by[0])
		return 1;
	snprintf(text, sizeof(text), _("%s is used by %s.\n\nThey will not load while it is disabled. "
		"Disable anyway?"), l->items[index].name, by);
	ui_keys(keys, sizeof(keys), &(struct ui_key_names){.ok = _("Disable"), .back = _("Back")});
	ui_screen(ui, TITLE, text, keys);
	return wait_ok(input, stop) == INPUT_OK;
}

static void toggle_item(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	struct list *l, int index)
{
	struct item *item = &l->items[index];
	char error[320];
	int done;
	if (item->state == ENABLED && !confirm_disable(ui, input, stop, l, index))
		return;
	if (item->state == MOVED || !l->blacklist) {
		if ((done = toggle(item, error, sizeof(error))))
			item->state = item->state == MOVED ? ENABLED : MOVED;
	} else if (item->state == ENABLED) {
		if ((done = blacklist_set(BLACKLIST_TEMPORARY, item->name, 1, error, sizeof(error))))
			item->state = TEMPORARY;
	} else if (item->state == TEMPORARY) {
		if ((done = blacklist_set(BLACKLIST, item->name, 1, error, sizeof(error)) &&
			blacklist_set(BLACKLIST_TEMPORARY, item->name, 0, error, sizeof(error))))
			item->state = BLACKLISTED;
	} else if ((done = blacklist_set(BLACKLIST, item->name, 0, error, sizeof(error)) &&
		blacklist_set(BLACKLIST_TEMPORARY, item->name, 0, error, sizeof(error))))
		item->state = ENABLED;
	if (!done) {
		ui_error(ui, TITLE, error);
		wait_ok(input, stop);
	}
}

void disable_plugins(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const char *system_cause)
{
	struct list *l = calloc(1, sizeof(*l));
	char (*labels)[300] = calloc(MAX_ITEMS, sizeof(*labels));
	const char **items = calloc(MAX_ITEMS, sizeof(*items));
	char *marks = calloc(MAX_ITEMS, 1);
	int problems;
	int selected;
	if (!l || !labels || !items || !marks)
		goto out;
	load_list(l);
	l->system_cause = system_cause;
	problems = mark_problems(l, marks, &selected);
	while (l->count && !(stop && *stop)) {
		enum input_key key;
		show_list(ui, l, labels, items, marks, problems, selected);
		key = input_next(input, 1000);
		selected = list_move(key, selected, l->count);
		if (key == INPUT_OK)
			toggle_item(ui, input, stop, l, selected);
		else if (key == INPUT_BACK || key == INPUT_RED)
			break;
	}
	if (l && !l->count) {
		ui_error(ui, TITLE, _("No plugins were found."));
		wait_ok(input, stop);
	}
out:
	free(marks);
	free(items);
	free(labels);
	free(l);
}

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
#define ENIGMA2_FILES "/var/lib/opkg/info/enigma2.list"  /* The own files of enigma2 are no add-ons. */
#define MAX_ITEMS 512

/* Plugins are folders, the other kinds single modules that plugins or skins install. */
struct kind {
	const char *folder;  /* Below PYTHON. */
	const char *disabled;  /* Below DISABLED_ROOT. */
	const char *type;  /* Translated where it is shown. */
	int files;
};

static const struct kind kinds[] = {
	{"Plugins/Extensions", "Extensions", N_("Plugin"), 0},
	{"Plugins/SystemPlugins", "SystemPlugins", N_("System plugin"), 0},
	{"Screens", "Screens", N_("Screen"), 1},
	{"Tools", "Tools", N_("Tool"), 1},
	{"Components/Converter", "Components/Converter", N_("Converter"), 1},
	{"Components/Renderer", "Components/Renderer", N_("Renderer"), 1},
};

struct item {
	int kind;
	char name[64];
	int disabled;
	char problem[160];
};

struct list {
	struct item items[MAX_ITEMS];
	int count;
	char *enigma2_files;  /* "\n" + the lines of enigma2.list, for strstr. */
};

static int is_directory(const char *path)
{
	struct stat info;
	return lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static char *read_file(const char *path, const char *prefix)
{
	FILE *file = fopen(path, "r");
	size_t length = strlen(prefix);
	size_t size = length + 1;
	char *text = malloc(size);
	char chunk[4096];
	size_t read_bytes;
	if (!text) {
		if (file)
			fclose(file);
		return NULL;
	}
	memcpy(text, prefix, length + 1);
	if (!file)
		return text;
	while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
		char *bigger = realloc(text, size + read_bytes);
		if (!bigger)
			break;
		text = bigger;
		memcpy(text + size - 1, chunk, read_bytes);
		size += read_bytes;
		text[size - 1] = '\0';
	}
	fclose(file);
	return text;
}

/* A line of the list is the path, a tab and the mode. */
static int of_enigma2(const struct list *l, const char *folder, const char *file)
{
	char line[256];
	const char *found;
	size_t length;
	if (!l->enigma2_files)
		return 0;
	length = (size_t)snprintf(line, sizeof(line), "\n" PYTHON "/%.40s/%.120s", folder, file);
	found = strstr(l->enigma2_files, line);
	while (found) {
		if (found[length] == '\t' || found[length] == '\n' || !found[length])
			return 1;
		found = strstr(found + length, line);
	}
	return 0;
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
	l->items[l->count].disabled = disabled;
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
		char name[64];
		size_t length = strlen(entry->d_name);
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "__pycache__") || length >= sizeof(name) + 4)
			continue;
		snprintf(path, sizeof(path), "%s/%.63s", folder, entry->d_name);
		if (!k->files) {
			if (is_directory(path))
				add(l, kind, entry->d_name, disabled);
			continue;
		}
		if (length > 4 && !strcmp(entry->d_name + length - 4, ".pyc"))
			length -= 4;
		else if (length > 3 && !strcmp(entry->d_name + length - 3, ".py"))
			length -= 3;
		else
			continue;
		snprintf(name, sizeof(name), "%.*s", (int)length, entry->d_name);
		if (!strcmp(name, "__init__") || (!disabled && of_enigma2(l, k->folder, entry->d_name)))
			continue;
		add(l, kind, name, disabled);
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

/* "No module named 'Tools.Weatherinfo'" of a module that is disabled here, -1 when not. */
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
		if (i >= 0 && l->items[i].disabled)
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
		if (kinds[k].files || strcmp(kinds[k].disabled, folder))
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

/* Python files of add-ons in the traceback of the newest crash log. */
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
			snprintf(name, sizeof(name), "%.*s", (int)strcspn(start + length + 1, kinds[k].files ? "./\"" : "/\""),
				start + length + 1);
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

static int toggle(const struct item *item, char *error, size_t size)
{
	const struct kind *k = &kinds[item->kind];
	char enabled[192];
	char disabled[192];
	static const char *const suffixes[] = {".py", ".pyc"};
	int moved = 0;
	snprintf(enabled, sizeof(enabled), PYTHON "/%s", k->folder);
	snprintf(disabled, sizeof(disabled), DISABLED_ROOT "/%s", k->disabled);
	make_folders(item->disabled ? enabled : disabled);
	if (!k->files) {
		char from[256];
		char to[256];
		snprintf(from, sizeof(from), "%s/%s", item->disabled ? disabled : enabled, item->name);
		snprintf(to, sizeof(to), "%s/%s", item->disabled ? enabled : disabled, item->name);
		return move(from, to, error, size);
	}
	for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); ++s) {
		char from[256];
		char to[256];
		struct stat info;
		snprintf(from, sizeof(from), "%s/%s%s", item->disabled ? disabled : enabled, item->name, suffixes[s]);
		snprintf(to, sizeof(to), "%s/%s%s", item->disabled ? enabled : disabled, item->name, suffixes[s]);
		if (lstat(from, &info) != 0)
			continue;
		if (!move(from, to, error, size))
			return 0;
		moved = 1;
	}
	if (!moved)
		snprintf(error, size, _("No file of %s was found."), item->name);
	return moved;
}

/* What an item is known by in other files: the module name and, for converters
 * and renderers, the attribute in skins. */
struct needle {
	char module[128];
	char skin[96];
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
	FILE *file = fopen(path, "rb");  /* NOSONAR a file of an installed plugin */
	struct stat info;
	char *data;
	int used = 0;
	if (!file)
		return 0;
	if (fstat(fileno(file), &info) != 0 || info.st_size <= 0 || info.st_size > 16 * 1024 * 1024) {
		fclose(file);
		return 0;
	}
	data = malloc((size_t)info.st_size);
	if (!data) {
		fclose(file);
		return 0;
	}
	if (fread(data, 1, (size_t)info.st_size, file) == (size_t)info.st_size)
		used = has_module(data, (size_t)info.st_size, n->module) ||
			(n->skin[0] && memmem(data, (size_t)info.st_size, n->skin, strlen(n->skin)));
	free(data);
	fclose(file);
	return used;
}

static int ends_with(const char *text, const char *end)
{
	size_t a = strlen(text);
	size_t b = strlen(end);
	return a >= b && !strcmp(text + a - b, end);
}

/* The .py, .pyc and .xml files below folder. */
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
		else if (ends_with(entry->d_name, ".py") || ends_with(entry->d_name, ".pyc") ||
			ends_with(entry->d_name, ".xml"))
			used = file_uses(path, n);
	}
	closedir(dir);
	return used;
}

static int item_uses(const struct item *item, const struct needle *n)
{
	const struct kind *k = &kinds[item->kind];
	char path[256];
	if (!k->files) {
		snprintf(path, sizeof(path), PYTHON "/%s/%s", k->folder, item->name);
		return folder_uses(path, n, 0);
	}
	snprintf(path, sizeof(path), PYTHON "/%s/%s.py", k->folder, item->name);
	if (file_uses(path, n))
		return 1;
	snprintf(path, sizeof(path), PYTHON "/%s/%s.pyc", k->folder, item->name);
	return file_uses(path, n);
}

/* The folder of config.skin.primary_skin, e.g. "MetrixHD". */
static int skin_folder(char *folder, size_t size)
{
	FILE *file = fopen("/etc/enigma2/settings", "r");
	char line[256];
	int found = 0;
	if (!file)
		return 0;
	while (!found && fgets(line, sizeof(line), file))
		if (!strncmp(line, "config.skin.primary_skin=", 25) && strchr(line + 25, '/')) {
			snprintf(folder, size, "%.*s", (int)strcspn(line + 25, "/"), line + 25);
			found = 1;
		}
	fclose(file);
	return found;
}

/* The module name and the skin attribute of item. */
static void make_needle(const struct item *item, struct needle *n)
{
	const struct kind *k = &kinds[item->kind];
	snprintf(n->module, sizeof(n->module), "%s.%s", k->folder, item->name);
	for (char *c = n->module; *c; ++c)
		if (*c == '/')
			*c = '.';
	n->skin[0] = '\0';
	if (!strcmp(k->type, "Converter"))
		snprintf(n->skin, sizeof(n->skin), "type=\"%s\"", item->name);
	else if (!strcmp(k->type, "Renderer"))
		snprintf(n->skin, sizeof(n->skin), "render=\"%s\"", item->name);
}

/* The enabled items and the skin that use item index, empty when none. */
static void users(const struct list *l, int index, char *text, size_t size)
{
	struct needle n;
	char skin[64];
	size_t used = 0;
	int count = 0;
	int more = 0;
	make_needle(&l->items[index], &n);
	text[0] = '\0';
	for (int i = 0; i < l->count; ++i) {
		if (i == index || l->items[i].disabled || !item_uses(&l->items[i], &n))
			continue;
		if (count++ < 6)
			used += (size_t)snprintf(text + used, size - used, "%s%s (%s)", used ? ", " : "",
				l->items[i].name, _(kinds[l->items[i].kind].type));
		else
			more++;
		if (used >= size)
			return;
	}
	if (n.skin[0] && skin_folder(skin, sizeof(skin))) {
		char folder[128];
		snprintf(folder, sizeof(folder), "/usr/share/enigma2/%s", skin);
		if (folder_uses(folder, &n, 0))
		{
			char part[96];
			snprintf(part, sizeof(part), _("the skin %s"), skin);
			used += (size_t)snprintf(text + used, size - used, "%s%s", used ? ", " : "", part);
		}
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
	l->enigma2_files = read_file(ENIGMA2_FILES, "\n");
	for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
		scan(l, (int)k, 0);
		scan(l, (int)k, 1);
	}
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

static void show_list(const struct ui_context *ui, const struct list *l, char (*labels)[300], const char **items,
	char *marks, int problems, int selected)
{
	char footer[128];
	char body[256];
	char title[128];
	int disabled = 0;
	char header[96];
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"),
		.ok = l->items[selected].disabled ? _("Enable") : _("Disable"), .back = _("Menu")});
	if (problems)
		snprintf(body, sizeof(body), "%s", ngettext("The plugin that caused the problem is marked. Changes take "
			"effect when Enigma2 starts again.", "The plugins that caused the problem are marked. Changes take "
			"effect when Enigma2 starts again.", problems));
	else
		snprintf(body, sizeof(body), "%s", _("Changes take effect when Enigma2 starts again."));
	snprintf(header, sizeof(header), "%s\t%s\t%s", _("Name"), _("Type"), _("Problem"));
	for (int i = 0; i < l->count; ++i) {
		char name[96];
		if (l->items[i].disabled)
			snprintf(name, sizeof(name), _("%s (disabled)"), l->items[i].name);
		else
			snprintf(name, sizeof(name), "%s", l->items[i].name);
		snprintf(labels[i], sizeof(labels[i]), "%s\t%s\t%s", name, _(kinds[l->items[i].kind].type),
			l->items[i].problem);
		items[i] = labels[i];
		if (l->items[i].problem[0])
			marks[i] = 1;  /* Yellow. */
		else if (l->items[i].disabled)
			marks[i] = 2;  /* Grey. */
		else
			marks[i] = 0;
	}
	for (int i = 0; i < l->count; ++i)
		disabled += l->items[i].disabled;
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
	char error[320];
	if (!l->items[index].disabled && !confirm_disable(ui, input, stop, l, index))
		return;
	if (toggle(&l->items[index], error, sizeof(error)))
		l->items[index].disabled = !l->items[index].disabled;
	else {
		ui_error(ui, TITLE, error);
		wait_ok(input, stop);
	}
}

void disable_plugins(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
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
	if (l)
		free(l->enigma2_files);
	free(marks);
	free(items);
	free(labels);
	free(l);
}

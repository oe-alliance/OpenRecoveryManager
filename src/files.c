#define _GNU_SOURCE

#include "files.h"

#include "i18n.h"
#include "viewer.h"

#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define TITLE _("Free up space")
#define MAX_ENTRIES 512
#define WALK_FLAGS (FTW_PHYS | FTW_MOUNT)  /* No symlinks followed, no other file system entered. */

struct entry {
	char name[256];
	long long size;  /* The blocks it takes, what deleting it frees. */
	int folder;
};

struct folder {
	char path[1024];
	struct entry entries[MAX_ENTRIES];
	int count;
};

static long long walked;  /* The sum of the walk, nftw passes no context. */

void files_size_text(long long bytes, char *text, size_t size)
{
	if (bytes >= 1024LL * 1024 * 1024)
		snprintf(text, size, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
	else if (bytes >= 1024LL * 1024)
		snprintf(text, size, "%lld MB", bytes / (1024 * 1024));
	else if (bytes >= 1024)
		snprintf(text, size, "%lld KB", bytes / 1024);
	else
		snprintf(text, size, "%lld B", bytes);
}

static int add_size(const char *path, const struct stat *info, int type, struct FTW *ftw)  /* NOSONAR the callback type of nftw */
{
	(void)path;
	(void)type;
	(void)ftw;
	walked += (long long)info->st_blocks * 512;
	return 0;
}

static long long tree_size(const char *path)
{
	walked = 0;
	nftw(path, add_size, 16, WALK_FLAGS);
	return walked;
}

static int remove_one(const char *path, const struct stat *info, int type, struct FTW *ftw)  /* NOSONAR the callback type of nftw */
{
	(void)info;
	(void)type;
	(void)ftw;
	return remove(path) != 0 ? errno : 0;  /* NOSONAR deleting what the user chose is the purpose */
}

static long long free_space(const char *path)
{
	struct statvfs fs;
	return statvfs(path, &fs) == 0 ? (long long)fs.f_bavail * (long long)fs.f_frsize : 0;
}

static int by_size(const void *a, const void *b)
{
	const struct entry *x = a;
	const struct entry *y = b;
	if (x->size != y->size)
		return x->size < y->size ? 1 : -1;
	return strcmp(x->name, y->name);
}

static void load(const struct ui_context *ui, struct folder *f)
{
	DIR *dir;
	const struct dirent *item;
	ui_progress(ui, TITLE, _("Measuring the folders and files."), 40, _("Please wait..."), _("Please wait..."));
	f->count = 0;
	dir = opendir(f->path);  /* NOSONAR a folder of the medium the user browses */
	if (!dir)
		return;
	while (f->count < MAX_ENTRIES) {
		struct entry *e = &f->entries[f->count];
		char path[1300];
		struct stat info;
		item = readdir(dir);
		if (!item)
			break;
		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", f->path, item->d_name);
		if (lstat(path, &info) != 0)
			continue;
		snprintf(e->name, sizeof(e->name), "%s", item->d_name);
		e->folder = S_ISDIR(info.st_mode);
		e->size = e->folder ? tree_size(path) : (long long)info.st_blocks * 512;
		f->count++;
	}
	closedir(dir);
	qsort(f->entries, (size_t)f->count, sizeof(f->entries[0]), by_size);
}

static void show(const struct ui_context *ui, const struct folder *f, const char *medium, long long need,
	int selected)
{
	static char rows[MAX_ENTRIES][300];
	const char *items[MAX_ENTRIES];
	char marks[MAX_ENTRIES];
	char body[1400];
	char header[96];
	char footer[192];
	char free_text[32];
	char need_text[32];
	const char *empty = _("This folder is empty.");
	const struct entry *chosen = f->count ? &f->entries[selected] : NULL;
	long long left = free_space(medium);
	files_size_text(left, free_text, sizeof(free_text));
	if (need > 0) {
		files_size_text(need, need_text, sizeof(need_text));
		snprintf(body, sizeof(body), left >= need ? _("%s\nFree %s, needed %s. That is enough now.") :
			_("%s\nFree %s, needed %s."), f->path, free_text, need_text);
	} else
		snprintf(body, sizeof(body), _("%s\nFree %s."), f->path, free_text);
	for (int i = 0; i < f->count; ++i) {
		char size_text[32];
		files_size_text(f->entries[i].size, size_text, sizeof(size_text));
		snprintf(rows[i], sizeof(rows[i]), "%s%s\t%s", f->entries[i].name, f->entries[i].folder ? "/" : "", size_text);
		items[i] = rows[i];
		marks[i] = 0;
	}
	if (!f->count) {
		items[0] = empty;
		marks[0] = 2;
	}
	snprintf(header, sizeof(header), "%s\t%s", _("Name"), _("Size"));
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"),
		.ok = chosen && chosen->folder ? _("Open") : NULL, .red = chosen ? _("Delete") : NULL, .back = _("Back")});
	ui_menu_table(ui, &(struct ui_menu){.title = TITLE, .body = body, .header = header, .items = items,
		.count = f->count ? f->count : 1, .selected = f->count ? selected : -1, .marks = marks, .marked = -1,
		.align = "lr", .footer = footer});
}

/* Deletes the chosen entry after a question, 1 when it is gone. */
static int delete_entry(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct folder *f, const struct entry *e)
{
	char path[1300];
	char size_text[32];
	char question[700];
	int failed;
	files_size_text(e->size, size_text, sizeof(size_text));
	if (e->folder)
		snprintf(question, sizeof(question), _("Delete the folder %s with everything in it (%s)? This cannot be undone."),
			e->name, size_text);
	else
		snprintf(question, sizeof(question), _("Delete %s (%s)? This cannot be undone."), e->name, size_text);
	if (!ask(ui, input, stop, TITLE, question, 0))
		return 0;
	snprintf(path, sizeof(path), "%s/%s", f->path, e->name);
	ui_progress(ui, TITLE, _("Deleting..."), 60, _("Please wait..."), _("Please wait..."));
	failed = e->folder ? nftw(path, remove_one, 16, WALK_FLAGS | FTW_DEPTH) : remove_one(path, NULL, 0, NULL);
	if (failed) {
		char text[1500];
		snprintf(text, sizeof(text), _("%s cannot be deleted completely: %s"), path, strerror(failed > 0 ? failed : errno));
		ui_error(ui, TITLE, text);
		while (input_next(input, 1000) != INPUT_OK && !(stop && *stop))
			;
	}
	return 1;
}

void files_free_space(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const char *medium, long long need)
{
	struct folder *f = calloc(1, sizeof(*f));
	int selected = 0;
	if (!f)
		return;
	snprintf(f->path, sizeof(f->path), "%s", medium);
	load(ui, f);
	while (!(stop && *stop)) {
		enum input_key key;
		show(ui, f, medium, need, selected);
		key = input_next(input, 1000);
		selected = list_move(key, selected, f->count);
		if (key == INPUT_OK && f->count && f->entries[selected].folder &&
			strlen(f->path) + strlen(f->entries[selected].name) + 2 < sizeof(f->path)) {
			strcat(strcat(f->path, "/"), f->entries[selected].name);
			selected = 0;
			load(ui, f);
		} else if (key == INPUT_RED && f->count && delete_entry(ui, input, stop, f, &f->entries[selected])) {
			load(ui, f);
			if (selected >= f->count)
				selected = f->count ? f->count - 1 : 0;
		} else if (key == INPUT_BACK) {
			char *slash = strrchr(f->path, '/');
			if (!strcmp(f->path, medium) || !slash)
				break;
			*slash = '\0';
			selected = 0;
			load(ui, f);
		}
	}
	free(f);
}

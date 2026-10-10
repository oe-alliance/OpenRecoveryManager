#define _GNU_SOURCE

#include "slots.h"

#include "i18n.h"
#include "process.h"
#include "viewer.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE _("Boot another slot")
#define REPOSITORY "oe-alliance/MultiBootSelectorPlugin"
#define DOWNLOADED "/tmp/multiboot-selector.sh"
#define MAX_SLOTS 16

struct slot {
	char id[8];  /* 1-4 for images, R for the recovery and so on, as in STARTUP_<id>. */
	char text[160];
};

struct listing {
	const struct ui_context *ui;
	struct slot slots[MAX_SLOTS];
	int count;
	int current;  /* The running slot, -1 when the script marks none. */
	char title[160];  /* With the kind of multiboot the script found, like the plugin. */
	char status[256];
};

/* STARTUP or cmdline.txt and a STARTUP_<slot> in dir, like loadBootDevice() and loadBootSlots() of MultiBoot.py. */
static int has_slots(const char *dir)
{
	char path[256];
	DIR *entries;
	int slots = 0;
	snprintf(path, sizeof(path), "%s/STARTUP", dir);
	if (access(path, F_OK) != 0) {
		snprintf(path, sizeof(path), "%s/cmdline.txt", dir);
		if (access(path, F_OK) != 0)
			return 0;
	}
	entries = opendir(dir);
	if (!entries)
		return 0;
	while (!slots) {
		const struct dirent *entry = readdir(entries);
		if (!entry)
			break;
		slots = strncmp(entry->d_name, "STARTUP_", 8) == 0;
	}
	closedir(entries);
	return slots;
}

/* Mounts device read-only for has_slots(), 0 when it cannot be. */
static int device_has_slots(const char *device)
{
	char dir[] = "/tmp/orm-boot-XXXXXX";
	char source[64];
	char *const mount_ro[] = {"mount", "-o", "ro", source, dir, NULL};
	int found = 0;
	if (access(device, F_OK) != 0 || !mkdtemp(dir))
		return 0;
	snprintf(source, sizeof(source), "%s", device);
	if (process_run(mount_ro, NULL, NULL, NULL) == 0) {
		found = has_slots(dir);
		umount(dir);
	}
	rmdir(dir);
	return found;
}

int slots_multiboot(void)
{
	static const char *const devices[] = {"/dev/mmcblk0p1", "/dev/mmcblk1p1", "/dev/mmcblk0p3", "/dev/mmcblk0p4",
		"/dev/mtdblock2", "/dev/block/by-name/bootoptions", "/dev/block/by-name/others", "/dev/block/by-name/startup",
		NULL};
	static const char *const vuplus[] = {"/dev/mmcblk0p4", "/dev/mmcblk0p7", "/dev/mmcblk0p9", NULL};
	static int found = -1;
	const char *const *device;
	char cmdline[1024] = "";
	FILE *file;
	if (found >= 0)
		return found;
	if (access("/data/bootconfig.txt", F_OK) == 0 || has_slots("/boot")) {  /* Dreambox, or mounted by the image. */
		found = 1;
		return found;
	}
	if ((file = fopen("/proc/cmdline", "r"))) {
		if (!fgets(cmdline, sizeof(cmdline), file))
			cmdline[0] = '\0';
		fclose(file);
	}
	found = 0;
	device = strstr(cmdline, "kexec=1") ? vuplus : devices;
	while (*device && !found) {
		found = device_has_slots(*device);
		++device;
	}
	return found;
}

static void wait_key(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
}

static void message(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const char *text)
{
	ui_error(ui, TITLE, text);
	wait_key(input, stop);
}

static int boot_mounted(void)
{
	char line[512];
	int found = 0;
	FILE *file = fopen("/proc/mounts", "r");
	if (!file)
		return 0;
	while (!found && fgets(line, sizeof(line), file))
		found = strstr(line, " /boot ") != NULL;
	fclose(file);
	return found;
}

static int download(const char *url, const char *path, char *output, size_t size)
{
	char tool[64];
	char source[256];
	char target[64];
	snprintf(source, sizeof(source), "%s", url);
	snprintf(target, sizeof(target), "%s", path ? path : "-");
	if (process_find("curl", tool, sizeof(tool))) {
		char *const curl[] = {tool, "-fsSL", "--max-time", "30", "-o", target, source, NULL};
		return path ? process_run(curl, NULL, NULL, NULL) == 0 : process_capture(curl, output, size) == 0;
	}
	if (process_find("wget", tool, sizeof(tool))) {
		char *const wget[] = {tool, "-q", "-T", "30", "-O", target, source, NULL};
		return path ? process_run(wget, NULL, NULL, NULL) == 0 : process_capture(wget, output, size) == 0;
	}
	return 0;
}

static int is_script(const char *path)
{
	char first[32] = "";
	FILE *file = fopen(path, "r");
	if (!file)
		return 0;
	if (!fgets(first, sizeof(first), file))
		first[0] = '\0';
	fclose(file);
	return strncmp(first, "#!/bin/bash", 11) == 0;
}

/* The script of the newest release, never a work in progress of the main branch. */
static int fetch_selector(const struct ui_context *ui, char *path, size_t size)
{
	char release[8192];
	char tag[64] = "";
	char url[256];
	const char *name;
	size_t i = 0;
	ui_progress(ui, TITLE, _("Loading the multiboot selector from GitHub."), 30,
		_("Looking for the newest release..."), _("Please wait..."));
	if (!download("https://api.github.com/repos/" REPOSITORY "/releases/latest", NULL, release, sizeof(release)))
		return 0;
	name = strstr(release, "\"tag_name\"");
	name = name ? strchr(name + 10, '"') : NULL;
	if (!name)
		return 0;
	for (++name; name[i] && name[i] != '"' && i < sizeof(tag) - 1; ++i) {
		if (!isalnum((unsigned char)name[i]) && !strchr("._-", name[i]))
			return 0;
		tag[i] = name[i];
	}
	tag[i] = '\0';
	if (!tag[0])
		return 0;
	snprintf(url, sizeof(url), "https://raw.githubusercontent.com/" REPOSITORY "/%s/src/usr/bin/multiboot-selector.sh", tag);
	ui_progress(ui, TITLE, _("Loading the multiboot selector from GitHub."), 60, tag, _("Please wait..."));
	unlink(DOWNLOADED);
	if (!download(url, DOWNLOADED, NULL, 0))
		return 0;
	if (!is_script(DOWNLOADED)) {
		unlink(DOWNLOADED);
		return 0;
	}
	chmod(DOWNLOADED, 0700);
	snprintf(path, size, "%s", DOWNLOADED);
	return 1;
}

/* "3) Slot eMMC: OpenATV 8.0.0-beta (2026-09-27) - Current". Like the plugin only
 * lines with "Slot", the script lists slots it cannot read without text. */
static void handle_line(const char *line, void *opaque)
{
	struct listing *l = opaque;
	const char *close = strstr(line, ") ");
	const char *text;
	size_t length;
	if (line[0])
		snprintf(l->status, sizeof(l->status), "%.255s", line);
	if (strncmp(line, "BOOT ", 5) == 0 && strchr(line, ':'))  /* "BOOT oem found: /dev/mmcblk0p4" */
		snprintf(l->title, sizeof(l->title), "%s - %.120s", TITLE, line);
	if (!close || close == line || close - line >= (int)sizeof(l->slots[0].id) || l->count == MAX_SLOTS)
		return;
	for (text = line; text < close; ++text)
		if (!isalnum((unsigned char)*text))
			return;
	text = close + 2;
	if (strncmp(text, "Slot ", 5) != 0)
		return;
	text += 5;
	length = strlen(text);
	if (length >= 10 && strcmp(text + length - 10, " - Current") == 0)
		l->current = l->count;
	snprintf(l->slots[l->count].id, sizeof(l->slots[0].id), "%.*s", (int)(close - line), line);
	snprintf(l->slots[l->count].text, sizeof(l->slots[0].text), "%s", text);
	l->count++;
}

static void tick(void *opaque)
{
	const struct listing *l = opaque;
	ui_progress(l->ui, TITLE, _("Looking for the images in the slots."), 50,
		l->status[0] ? l->status : _("Please wait..."), _("Please wait..."));
}

static int choose(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct listing *l)
{
	char labels[MAX_SLOTS][176];
	const char *items[MAX_SLOTS];
	int selected = l->current >= 0 ? l->current : 0;  /* Like the plugin. */
	char footer[128];
	while (!(stop && *stop)) {
		enum input_key key;
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Boot"),
			.back = _("Menu")});
		for (int i = 0; i < l->count; ++i) {
			snprintf(labels[i], sizeof(labels[i]), _("Slot %s: %s"), l->slots[i].id, l->slots[i].text);
			items[i] = labels[i];
		}
		ui_menu(ui, l->title, _("Which slot should the receiver start?"), items, l->count,
			selected, footer);
		key = input_next(input, 1000);
		selected = list_move(key, selected, l->count);
		if (key == INPUT_OK)
			return selected;
		else if (key == INPUT_BACK || key == INPUT_RED)
			return -1;
	}
	return -1;
}

int boot_slot(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	char selector[128] = DOWNLOADED;
	struct listing *l;
	int mounted = boot_mounted();
	int choice;
	int result;
	/* One in /tmp first, e.g. loaded before or put there by a supporter, then the one of the image. */
	if (!is_script(selector))
		snprintf(selector, sizeof(selector), "%s", MULTIBOOT_SELECTOR);
	if (access(selector, R_OK) != 0 && !fetch_selector(ui, selector, sizeof(selector))) {
		message(ui, input, stop, _("The multiboot selector could not be loaded from GitHub. "
			"The receiver needs a network connection for it."));
		return 0;
	}
	l = calloc(1, sizeof(*l));
	if (!l)
		return 0;
	l->ui = ui;
	l->current = -1;
	snprintf(l->title, sizeof(l->title), "%s", TITLE);
	{
		char *const list[] = {"bash", selector, "list", NULL};
		result = process_run_with_updates(list, NULL, handle_line, tick, 300, l);
	}
	if (!mounted && boot_mounted())
		umount("/boot");  /* The list leaves the boot partition mounted. */
	if (result != 0) {
		message(ui, input, stop, l->status[0] ? l->status : _("The slots could not be read."));
		free(l);
		return 0;
	}
	if (!l->count) {
		message(ui, input, stop, _("There is no slot to boot."));
		free(l);
		return 0;
	}
	choice = choose(ui, input, stop, l);
	if (choice < 0) {
		free(l);
		return 0;
	}
	ui_progress(ui, TITLE, _("Switching to the slot."), 80, l->slots[choice].text, _("Please wait..."));
	{
		char *const select[] = {"bash", selector, l->slots[choice].id, NULL};
		l->status[0] = '\0';
		result = process_run(select, NULL, handle_line, l);
	}
	if (result != 0) {
		message(ui, input, stop, l->status[0] ? l->status : _("The slot could not be switched."));
		free(l);
		return 0;
	}
	free(l);
	return 1;
}

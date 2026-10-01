#define _GNU_SOURCE

#include "update.h"

#include "boxinfo.h"
#include "i18n.h"
#include "process.h"
#include "viewer.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define TITLE _("Software update")
#define UPDATE_LIMIT 200  /* BoxInfo "UpdateLimit" of enigma2, above it flashing is safer. */
#define MAX_PACKAGES 1000
#define MAX_ARGS 64
#define UPGRADE_LOG "/home/root/ipkgupgrade.log"
#define BUSYBOX_MARKER "/etc/enigma2/.busybox_update_required"  /* enigma2.sh reinstalls busybox. */

enum feed { FEED_UNKNOWN, FEED_RED, FEED_YELLOW, FEED_GREEN };

struct packages {
	char *upgrade[MAX_PACKAGES];  /* "name\tinstalled\tnew" for the list. */
	char *held[MAX_PACKAGES];  /* Names left out like in enigma2. */
	int upgrade_count;
	int held_count;
	char overwrite[8][8];  /* Settings, drivers, emus, picons, bootlogo, spinner, config files. */
	char distro[32];
};

static void wait_key(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
}

static void message(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const char *text, int error)
{
	char footer[64];
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = _("Menu")});
	if (error)
		ui_error(ui, TITLE, text);
	else
		ui_screen(ui, TITLE, text, footer);
	wait_key(input, stop);
}

static int download(char *url, char *output, size_t size)
{
	char tool[64];
	if (process_find("curl", tool, sizeof(tool))) {
		char *const curl[] = {tool, "-fsSL", "--max-time", "10", url, NULL};
		return process_capture(curl, output, size) == 0;
	}
	if (process_find("wget", tool, sizeof(tool))) {
		char *const wget[] = {tool, "-q", "-T", "10", "-O", "-", url, NULL};
		return process_capture(wget, output, size) == 0;
	}
	return 0;
}

/* The traffic light of the feed like SoftwareUpdate of enigma2; red is yellow on a beta image. */
static enum feed feed_status(void)
{
	char box[64];
	char url[160];
	char page[16384];
	boxinfo_box_name(box, sizeof(box));
	snprintf(url, sizeof(url), "https://ampel.mynonpublic.com/status/index.php?boxname=%s", box);
	if (!box[0] || !download(url, page, sizeof(page)))
		return FEED_UNKNOWN;
	if (strstr(page, "rot.png"))
		return access("/etc/.beta", F_OK) == 0 ? FEED_YELLOW : FEED_RED;
	if (strstr(page, "gelb.png"))
		return FEED_YELLOW;
	if (strstr(page, "gruen.png"))
		return FEED_GREEN;
	return FEED_UNKNOWN;
}

/* config.plugins.softwaremanager of enigma2, which saves only changed values. */
static void read_settings(struct packages *p)
{
	static const char *const keys[] = {"overwriteSettingsFiles", "overwriteDriversFiles",
		"overwriteEmusFiles", "overwritePiconsFiles", "overwriteBootlogoFiles",
		"overwriteSpinnerFiles", "overwriteConfigFiles"};
	static const char *const defaults[] = {"False", "True", "True", "True", "True", "True", "Y"};
	char line[256];
	FILE *file;
	int i;
	for (i = 0; i < 7; ++i)
		snprintf(p->overwrite[i], sizeof(p->overwrite[i]), "%s", defaults[i]);
	boxinfo_value("distro", p->distro, sizeof(p->distro));
	file = fopen("/etc/enigma2/settings", "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		text_cut(line, "\r\n");
		if (strncmp(line, "config.plugins.softwaremanager.", 31) != 0)
			continue;
		for (i = 0; i < 7; ++i) {
			size_t length = strlen(keys[i]);
			if (strncmp(line + 31, keys[i], length) == 0 && line[31 + length] == '=')
				snprintf(p->overwrite[i], sizeof(p->overwrite[i]), "%.7s", line + 32 + length);
		}
	}
	fclose(file);
}

static int off(const struct packages *p, int index)
{
	return strcasecmp(p->overwrite[index], "true") != 0;
}

static int excluded(const struct packages *p, const char *name)  /* isExcluded() of Components/Opkg.py */
{
	char spinner[48];
	snprintf(spinner, sizeof(spinner), "%s-spinner", p->distro[0] ? p->distro : "openatv");
	return strstr(name, "busybox") ||
		(strstr(name, "-settings-") && off(p, 0)) ||
		(strstr(name, "kernel-module-") && off(p, 1)) ||
		(strstr(name, "-softcams-") && off(p, 2)) ||
		(strstr(name, "-picons-") && off(p, 3)) ||
		(strstr(name, "-bootlogo") && off(p, 4)) ||
		(strstr(name, spinner) && off(p, 5));
}

/* Without the git hashes: 8.0.0+git35563+5370b4a0+5370b4a535-r1 -> 8.0.0+git35563-r1 */
static void drop_hashes(const char *version, char *out, size_t size)
{
	size_t used = 0;
	while (*version && used + 1 < size) {
		if (*version == '+') {
			size_t hex = strspn(version + 1, "0123456789abcdef");
			char after = version[1 + hex];
			if (hex >= 7 && (!after || after == '+' || after == '-')) {
				version += 1 + hex;
				continue;
			}
		}
		out[used++] = *version++;
	}
	out[used] = '\0';
}

static int separator(char c)
{
	return c == '+' || c == '-' || c == '_' || c == '~';
}

/* Only what differs: the same start and end of both versions go, cut at a separator. */
static void shorten(const char *old_version, const char *new_version, char *old_out,
	char *new_out, size_t size)
{
	char a[128];
	char b[128];
	size_t la;
	size_t lb;
	size_t prefix = 0;
	size_t suffix = 0;
	size_t cut = 0;
	drop_hashes(old_version, a, sizeof(a));
	drop_hashes(new_version, b, sizeof(b));
	la = strlen(a);
	lb = strlen(b);
	while (prefix < la && prefix < lb && a[prefix] == b[prefix]) {
		if (separator(a[prefix]))
			cut = prefix + 1;
		prefix++;
	}
	prefix = cut < la && cut < lb ? cut : 0;
	cut = 0;
	while (suffix < la - prefix && suffix < lb - prefix && a[la - 1 - suffix] == b[lb - 1 - suffix]) {
		if (separator(a[la - 1 - suffix]))
			cut = suffix + 1;
		suffix++;
	}
	suffix = la - prefix > cut && lb - prefix > cut ? cut : 0;
	snprintf(old_out, size, "%.*s", (int)(la - prefix - suffix), a + prefix);
	snprintf(new_out, size, "%.*s", (int)(lb - prefix - suffix), b + prefix);
}

/* "name - old - new" of opkg list-upgradable, as columns for the list. */
static void handle_upgradable(const char *line, void *opaque)
{
	struct packages *p = opaque;
	char name[128];
	const char *old;
	const char *new;
	char entry[400];
	if (!line[0] || line[0] == ' ' || !strncmp(line, "Not selecting ", 14) || !strncmp(line, "error: ", 7))
		return;
	old = strstr(line, " - ");
	if (!old || old - line >= (int)sizeof(name))
		return;
	snprintf(name, sizeof(name), "%.*s", (int)(old - line), line);
	old += 3;
	new = strstr(old, " - ");
	if (excluded(p, name)) {
		if (p->held_count < MAX_PACKAGES)
			p->held[p->held_count++] = strdup(name);
		return;
	}
	if (new) {
		char installed[128];
		char available[128];
		char old_version[128];
		snprintf(old_version, sizeof(old_version), "%.*s", (int)(new - old), old);
		shorten(old_version, new + 3, installed, available, sizeof(installed));
		snprintf(entry, sizeof(entry), "%s\t%s\t%s", name, installed, available);
	} else
		snprintf(entry, sizeof(entry), "%s\t\t%s", name, old);
	if (p->upgrade_count < MAX_PACKAGES)
		p->upgrade[p->upgrade_count++] = strdup(entry);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* --add-dest for packages installed on media, like upgrade.py of enigma2. */
static int add_destinations(char *argv[], int argc, char storage[][64], int max)
{
	DIR *dir = opendir("/media");
	struct dirent *entry;
	int used = 0;
	if (!dir)
		return argc;
	while ((entry = readdir(dir)) && used < max && argc < MAX_ARGS - 4) {
		char status[160];
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "net") || strlen(entry->d_name) > 24)
			continue;
		snprintf(status, sizeof(status), "/media/%.24s/var/lib/opkg/status", entry->d_name);
		if (access(status, F_OK) != 0)
			continue;
		snprintf(storage[used], 64, "/media/%.24s:/media/%.24s", entry->d_name, entry->d_name);
		argv[argc++] = "--add-dest";
		argv[argc++] = storage[used++];
	}
	closedir(dir);
	return argc;
}

static void flag(char *const names[], int count, char *state, struct live_output *output)
{
	char **argv;
	if (!count)
		return;
	argv = calloc((size_t)count + 4, sizeof(*argv));
	if (!argv)
		return;
	argv[0] = "opkg";
	argv[1] = "flag";
	argv[2] = state;
	for (int i = 0; i < count; ++i)
		argv[3 + i] = names[i];
	process_run_with_updates(argv, NULL, live_output_line, live_output_tick, 100, output);
	free(argv);
}

static void free_packages(struct packages *p)
{
	int i;
	for (i = 0; i < p->upgrade_count; ++i)
		free(p->upgrade[i]);
	for (i = 0; i < p->held_count; ++i)
		free(p->held[i]);
	free(p);
}

static int confirm(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct packages *p, enum feed feed)
{
	static const char *const feeds[] = {N_("feed status unknown"), N_("feed disabled"), N_("feed unstable"),
		N_("feed stable")};
	char **lines = calloc((size_t)p->upgrade_count + 3, sizeof(*lines));
	char title[192];
	char packages[64];
	char warning[160];
	char header[96];
	char footer[128];
	int count = 0;
	int first = 0;
	enum input_key key;
	if (!lines)
		return 0;
	if (p->upgrade_count > UPDATE_LIMIT) {
		snprintf(warning, sizeof(warning), _("Warning: more than %d packages, flashing a new image is safer."),
			UPDATE_LIMIT);
		lines[count++] = warning;
		lines[count++] = "";
	}
	for (int i = 0; i < p->upgrade_count; ++i)
		lines[count++] = p->upgrade[i];
	snprintf(packages, sizeof(packages), ngettext("%d package", "%d packages", p->upgrade_count), p->upgrade_count);
	snprintf(title, sizeof(title), "%s, %s, %s", TITLE, packages, _(feeds[feed]));
	snprintf(header, sizeof(header), "%s\t%s\t%s", _("Package"), _("Installed"), _("New"));
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Scroll"), .ok = _("Update now"),
		.back = _("Menu")});
	do
		key = text_view(ui, input, stop, &(struct text_page){.title = title, .header = header, .lines = lines,
			.count = count, .first = &first, .empty = "", .footer = footer});
	while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && key != INPUT_NONE);
	free(lines);
	return key == INPUT_OK;
}

void update_packages(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	struct live_output output = {.ui = ui};
	struct packages *p;
	enum feed feed;
	char destinations[8][64];
	char *argv[MAX_ARGS];
	char title[128];
	char footer[256];
	int argc = 0;
	int result;
	int i;
	ui_progress(ui, TITLE, _("Checking the status of the feed."), 10, _("Please wait..."), _("Please wait..."));
	feed = feed_status();
	if (feed == FEED_RED) {
		message(ui, input, stop, _("The feed of this image is being updated or is disabled right now. "
			"Please try the update again later."), 1);
		return;
	}
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Updating the package lists"));
	output.title = title;
	output.footer = _("Please wait...");
	{
		char *const update[] = {"opkg", "update", NULL};
		result = process_run_with_updates(update, NULL, live_output_line, live_output_tick, 100, &output);
	}
	if (result != 0) {  /* Like enigma2 an unavailable extra feed does not matter when openatv-all is there. */
		for (i = 0; i < output.count; ++i)
			if (strstr(output.lines[i], "Updated source") && strstr(output.lines[i], "openatv-all"))
				result = 0;
	}
	if (result != 0) {
		live_output_view(&output, input, stop, _("The package lists could not be updated."), 0);
		live_output_free(&output);
		return;
	}
	live_output_free(&output);
	p = calloc(1, sizeof(*p));
	if (!p)
		return;
	read_settings(p);
	ui_progress(ui, TITLE, _("Looking for updates."), 50, _("Please wait..."), _("Please wait..."));
	{
		char *const list[] = {"opkg", "list-upgradable", NULL};
		process_run(list, NULL, handle_upgradable, p);
	}
	if (!p->upgrade_count) {
		message(ui, input, stop, _("There are no updates available."), 0);
		free_packages(p);
		return;
	}
	qsort(p->upgrade, (size_t)p->upgrade_count, sizeof(p->upgrade[0]), by_name);
	if (!confirm(ui, input, stop, p, feed)) {
		free_packages(p);
		return;
	}
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Updating"));
	output.title = title;
	output.footer = _("Please wait, do not switch off the receiver...");
	output.log = fopen(UPGRADE_LOG, "w");
	flag(p->held, p->held_count, "hold", &output);  /* Like the Opkg component of enigma2. */
	argv[argc++] = "opkg";
	if (!strcmp(p->overwrite[6], "Y"))
		argv[argc++] = "--force-maintainer";  /* The configuration files of the packages. */
	argc = add_destinations(argv, argc, destinations, 8);
	argv[argc++] = "upgrade";
	argv[argc] = NULL;
	result = process_run_with_updates(argv, NULL, live_output_line, live_output_tick, 100, &output);
	flag(p->held, p->held_count, "ok", &output);
	for (i = 0; i < p->held_count; ++i)
		if (strstr(p->held[i], "busybox")) {
			FILE *marker = fopen(BUSYBOX_MARKER, "w");
			if (marker) {
				fprintf(marker, "opkg install --force-reinstall busybox\n");
				fclose(marker);
			}
			break;
		}
	if (output.log)
		fclose(output.log);
	if (result == 0)
		snprintf(footer, sizeof(footer), "%s", _("The update is finished, restart the receiver in the menu."));
	else
		snprintf(footer, sizeof(footer), _("The update failed, see %s."), UPGRADE_LOG);
	live_output_view(&output, input, stop, footer, result == 0);
	live_output_free(&output);
	free_packages(p);
}

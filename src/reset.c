#define _GNU_SOURCE

#include "reset.h"

#include "i18n.h"
#include "viewer.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TITLE _("Reset settings")
#define CONFIG "/etc/enigma2"
#define DEFAULTS "/usr/share/enigma2/defaults"  /* Like the factory reset of enigma2. */

enum mode { SKIN, SETTINGS, EVERYTHING };
static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb");
	FILE *out;
	char buffer[4096];
	size_t length;
	int ok = 1;
	if (!in)
		return 0;
	if (!(out = fopen(to, "wb"))) {
		fclose(in);
		return 0;
	}
	while ((length = fread(buffer, 1, sizeof(buffer), in)) > 0)
		ok &= fwrite(buffer, 1, length, out) == length;  /* NOSONAR the default settings of the image */
	fclose(in);
	return (fclose(out) == 0) & ok;
}

/* The defaults of the image, but not the network: remote support needs it. */
static void install_defaults(void)
{
	DIR *dir = opendir(DEFAULTS);
	struct dirent *entry;
	if (!dir)
		return;
	while ((entry = readdir(dir))) {
		char from[512];
		char to[512];
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "interfaces"))
			continue;
		snprintf(from, sizeof(from), DEFAULTS "/%.255s", entry->d_name);
		snprintf(to, sizeof(to), CONFIG "/%.255s", entry->d_name);
		copy_file(from, to);
	}
	closedir(dir);
}

/* Like "Clean" in the FlashManager of enigma2: without the empty flags of the last online flash
 * neither FastRestore nor enigma2 restores a backup on their own, the wizard offers it. */
void remove_restore_flags(void)
{
	static const char *const flags[] = {"settings", "plugins", "noplugins", "slow", "fast", "turbo"};
	DIR *media = opendir("/media");
	struct dirent *entry;
	if (!media)
		return;
	while ((entry = readdir(media))) {
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "audiocd") || !strcmp(entry->d_name, "autofs"))
			continue;
		for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
			char path[512];
			struct stat info;
			snprintf(path, sizeof(path), "/media/%.200s/images/config/%s", entry->d_name, flags[i]);
			if (stat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size == 0)
				unlink(path);
		}
	}
	closedir(media);
}

/* settings without the skins, the old file and the user skins of enigma2 go to kept. */
static int reset_skin(const char *kept, char *error, size_t error_size)
{
	char line[1024];
	char path[256];
	FILE *in;
	FILE *out;
	DIR *dir;
	struct dirent *entry;
	snprintf(path, sizeof(path), "%s/settings", kept);
	if (!copy_file(CONFIG "/settings", path) && errno != ENOENT) {
		snprintf(error, error_size, _("%s cannot be saved: %s"), CONFIG "/settings", strerror(errno));
		return 0;
	}
	if ((in = fopen(CONFIG "/settings", "r"))) {
		if (!(out = fopen(CONFIG "/settings.new", "w"))) {  /* NOSONAR the settings of enigma2 */
			snprintf(error, error_size, _("%s cannot be created: %s"), CONFIG "/settings.new", strerror(errno));
			fclose(in);
			return 0;
		}
		while (fgets(line, sizeof(line), in))
			if (strncmp(line, "config.skin.primary_skin=", 25) && strncmp(line, "config.skin.display_skin=", 25))
				fputs(line, out);
		fclose(in);
		if (fclose(out) != 0 || rename(CONFIG "/settings.new", CONFIG "/settings") != 0) {
			snprintf(error, error_size, _("%s cannot be changed: %s"), CONFIG "/settings", strerror(errno));
			return 0;
		}
	}
	if ((dir = opendir(CONFIG))) {
		while ((entry = readdir(dir)))
			if (!strncmp(entry->d_name, "skin_user", 9) && strstr(entry->d_name, ".xml")) {
				char from[512];
				char to[512];
				snprintf(from, sizeof(from), CONFIG "/%.255s", entry->d_name);
				snprintf(to, sizeof(to), "%s/%.255s", kept, entry->d_name);
				rename(from, to);  /* NOSONAR moving the user skin aside is the reset */
			}
		closedir(dir);
	}
	return 1;
}

/* Only the skin, the file settings or all of CONFIG go to a folder next to it, nothing is deleted. */
static int reset(enum mode mode, char *kept, size_t size, char *error, size_t error_size)
{
	char stamp[32];
	struct tm local;
	time_t now = time(NULL);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime_r(&now, &local));
	snprintf(kept, size, CONFIG "-reset-%s", stamp);
	if (mode == EVERYTHING) {
		if (rename(CONFIG, kept) != 0 || mkdir(CONFIG, 0755) != 0) {  /* NOSONAR moving the settings aside is the reset */
			snprintf(error, error_size, _("%s cannot be moved: %s"), CONFIG, strerror(errno));
			return 0;
		}
		install_defaults();
		remove_restore_flags();
		return 1;
	}
	{
		char to[256];
		if (mkdir(kept, 0755) != 0) {
			snprintf(error, error_size, _("%s cannot be created: %s"), kept, strerror(errno));
			return 0;
		}
		if (mode == SKIN)
			return reset_skin(kept, error, error_size);
		snprintf(to, sizeof(to), "%s/settings", kept);
		if (rename(CONFIG "/settings", to) != 0 && errno != ENOENT) {
			snprintf(error, error_size, _("%s cannot be moved: %s"), CONFIG "/settings", strerror(errno));
			return 0;
		}
	}
	remove_restore_flags();
	return 1;
}

static void wait_ok(struct input_context *input, const volatile sig_atomic_t *stop)
{
	enum input_key key;
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && !(stop && *stop));
}

void reset_settings(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	const char *items[3];
	char body[512];
	char footer[128];
	int selected = 0;
	while (!(stop && *stop)) {
		enum input_key key;
		items[SKIN] = _("Only the skin (all other settings stay)");
		items[SETTINGS] = _("Only the settings (channel lists and timers stay)");
		items[EVERYTHING] = _("Everything, including channel lists and timers");
		snprintf(body, sizeof(body), _("What should be reset? After the settings or everything, Enigma2 starts with "
			"the wizard, which offers to restore a backup. The files are kept in a folder next to %s, the network "
			"settings stay."), CONFIG);
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Reset"),
			.back = _("Menu")});
		ui_menu(ui, TITLE, body, items, 3, selected, footer);
		key = input_next(input, 1000);
		selected = list_move(key, selected, 3);
		if (key == INPUT_OK) {
			static const char *const questions[] = {
				N_("Reset the skin now? Enigma2 then starts with its standard skin, all other settings stay. Your "
					"old settings are kept."),
				N_("Reset the settings now? Your channel lists and timers stay, all other settings start from the "
					"beginning. Your old settings are kept."),
				N_("Reset everything now? Enigma2 then starts like new, without channel lists and timers. Your old "
					"files are kept.")
			};
			char kept[128];
			char text[512];
			if (!ask(ui, input, stop, TITLE, _(questions[selected]), 0))
				continue;
			if (!reset((enum mode)selected, kept, sizeof(kept), text, sizeof(text))) {
				ui_error(ui, TITLE, text);
				wait_ok(input, stop);
				return;
			}
			if (selected == SKIN)
				snprintf(text, sizeof(text), _("The skin was reset, the old settings are kept in %s."), kept);
			else if (selected == SETTINGS)
				snprintf(text, sizeof(text), _("The settings were reset, the old file is kept in %s."), kept);
			else
				snprintf(text, sizeof(text), _("The files of Enigma2 were reset, the old ones are kept in %s."), kept);
			snprintf(text + strlen(text), sizeof(text) - strlen(text), "\n\n%s", selected == SKIN ?
				_("Restart Enigma2 in the menu to start with the standard skin.") :
				_("Restart Enigma2 in the menu to start with the wizard."));
			ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = _("Menu")});
			ui_screen(ui, TITLE, text, footer);
			wait_ok(input, stop);
			return;
		} else if (key == INPUT_BACK)
			return;
	}
}

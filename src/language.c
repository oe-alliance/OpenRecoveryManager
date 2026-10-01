#define _GNU_SOURCE

#include "language.h"

#include "i18n.h"
#include "viewer.h"

#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The catalog folder, the locale and the name in the language itself. Languages whose script the
 * fonts lack are left out. */
struct language {
	const char *code;
	const char *locale;
	const char *name;
};

static const struct language languages[] = {
	{"en", "en_US", "English"},
	{"ca", "ca_ES", "Català"},
	{"cs", "cs_CZ", "Čeština"},
	{"da", "da_DK", "Dansk"},
	{"de", "de_DE", "Deutsch"},
	{"et", "et_EE", "Eesti"},
	{"es", "es_ES", "Español"},
	{"fr", "fr_FR", "Français"},
	{"hr", "hr_HR", "Hrvatski"},
	{"it", "it_IT", "Italiano"},
	{"lv", "lv_LV", "Latviešu"},
	{"lt", "lt_LT", "Lietuvių"},
	{"hu", "hu_HU", "Magyar"},
	{"nl", "nl_NL", "Nederlands"},
	{"nb", "nb_NO", "Norsk"},
	{"pl", "pl_PL", "Polski"},
	{"pt", "pt_PT", "Português"},
	{"pt_BR", "pt_BR", "Português (Brasil)"},
	{"sk", "sk_SK", "Slovenčina"},
	{"sl", "sl_SI", "Slovenščina"},
	{"fi", "fi_FI", "Suomi"},
	{"sv", "sv_SE", "Svenska"},
	{"tr", "tr_TR", "Türkçe"},
	{"el", "el_GR", "Ελληνικά"},
	{"ar", "ar_EG", "العربية"},
	{"be", "be_BY", "Беларуская"},
	{"bg", "bg_BG", "Български"},
	{"mk", "mk_MK", "Македонски"},
	{"ru", "ru_RU", "Русский"},
	{"sr", "sr_RS", "Српски"},
	{"uk", "uk_UA", "Українська"},
};
#define LANGUAGE_COUNT (int)(sizeof(languages) / sizeof(languages[0]))

static int has_catalog(const struct language *language)
{
	char path[256];
	if (!strcmp(language->code, "en"))
		return 1;  /* The texts in the code. */
	snprintf(path, sizeof(path), LOCALEDIR "/%s/LC_MESSAGES/orm.mo", language->code);
	return access(path, R_OK) == 0;
}

/* The catalog and the locale are there, without switching to the language. */
static int usable(const struct language *language)
{
	locale_t locale;
	if (!has_catalog(language))
		return 0;
	if (!strcmp(language->code, "en"))
		return 1;
	if (!(locale = newlocale(LC_MESSAGES_MASK, language->locale, (locale_t)0)))
		return 0;
	freelocale(locale);
	return 1;
}

int language_other(void)
{
	for (int i = 1; i < LANGUAGE_COUNT; ++i)  /* Past English. */
		if (usable(&languages[i]))
			return 1;
	return 0;
}

/* The language of a locale like "de_DE" or "pt_BR.UTF-8": the one with that locale, else its language. */
static int find(const char *locale)
{
	int i;
	for (i = 0; i < LANGUAGE_COUNT; ++i)
		if (!strncmp(locale, languages[i].locale, strlen(languages[i].locale)))
			return i;
	for (i = 0; i < LANGUAGE_COUNT; ++i)
		if (!strncmp(locale, languages[i].code, 2))
			return i;
	return 0;
}

void language_choose(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop)
{
	const char *items[LANGUAGE_COUNT];
	int index[LANGUAGE_COUNT];
	char footer[128];
	int count = 0;
	int current = 0;
	int selected = 0;
	int running = find(i18n_locale());
	for (int i = 0; i < LANGUAGE_COUNT; ++i)
		if (usable(&languages[i])) {
			if (i == running)
				current = count;
			index[count] = i;
			items[count++] = languages[i].name;
		}
	selected = current;
	while (!(stop && *stop)) {
		enum input_key key;
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Choose"),
			.back = _("Back")});
		ui_menu_marked(ui, &(struct ui_menu){.title = _("Language"), .items = items, .count = count,
			.selected = selected, .marked = current, .footer = footer});
		key = input_next(input, 1000);
		selected = list_move(key, selected, count);
		if (key == INPUT_OK && i18n_set(languages[index[selected]].locale))
			current = selected;
		else if (key == INPUT_BACK)
			return;
	}
}

#define _GNU_SOURCE

#include "about.h"

#include "i18n.h"
#include "licenses.h"
#include "version.h"
#include "viewer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE _("About")
#define COPYRIGHT "Copyright \xC2\xA9 2026 OE-Alliance Open Recovery Manager contributors"
#define WRAP 100  /* Characters, the text view cuts longer lines. */
#define COUNT (int)(sizeof(licenses) / sizeof(licenses[0]))

/* The text in lines of at most WRAP characters, wrapped at spaces. */
static int wrap_lines(const char *text, char ***out)
{
	char **lines = NULL;
	int count = 0;
	while (*text) {
		size_t length = strcspn(text, "\n");
		char **grown;
		if (length > WRAP) {
			size_t space = WRAP;
			while (space > 0 && text[space] != ' ')
				space--;
			length = space ? space : WRAP;
		}
		if (!(grown = realloc(lines, (size_t)(count + 1) * sizeof(*lines))))
			break;
		lines = grown;
		lines[count++] = strndup(text, length);
		text += length;
		if (*text == '\n' || *text == ' ')
			text++;
	}
	*out = lines;
	return count;
}

static void show_license(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const struct license *license)
{
	char **lines;
	int count = wrap_lines(license->text, &lines);
	int first = 0;
	char footer[96];
	enum input_key key;
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Scroll"), .back = _("Back")});
	do
		key = text_view(ui, input, stop, &(struct text_page){.title = license->name, .lines = lines, .count = count,
			.first = &first, .empty = _("The license is empty."), .footer = footer});
	while (key != INPUT_BACK && key != INPUT_OK && key != INPUT_NONE);
	for (int i = 0; i < count; ++i)
		free(lines[i]);
	free(lines);
}

void about(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop)
{
	char rows[COUNT][160];
	const char *items[COUNT];
	char footer[160];
	char body[320];
	char header[128];
	int selected = 0;
	while (!(stop && *stop)) {
		enum input_key key;
		for (int i = 0; i < COUNT; ++i) {
			snprintf(rows[i], sizeof(rows[i]), "%s\t%s\t%s", licenses[i].name, _(licenses[i].role), licenses[i].license);
			items[i] = rows[i];
		}
		snprintf(body, sizeof(body), "Open Recovery Manager (ORM) %s\n" COPYRIGHT "\n\n%s", orm_version(),
			_("Free software under the GPLv3. OK shows the full license of each part."));
		snprintf(header, sizeof(header), "%s\t%s\t%s", _("Component"), _("Part"), _("License"));
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("License"),
			.back = _("Back")});
		ui_menu_table(ui, &(struct ui_menu){.title = TITLE, .body = body, .header = header, .items = items,
			.count = COUNT, .selected = selected, .marked = -1, .footer = footer});
		key = input_next(input, 1000);
		selected = list_move(key, selected, COUNT);
		if (key == INPUT_OK)
			show_license(ui, input, stop, &licenses[selected]);
		else if (key == INPUT_BACK)
			return;
	}
}

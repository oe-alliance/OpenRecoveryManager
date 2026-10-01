#define _GNU_SOURCE

#include "viewer.h"

#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LIVE_MAX_LINES 4000

enum input_key text_view(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct text_page *page)
{
	const char *const nothing[] = {page->empty};
	int count = page->count;
	int *first = page->first;
	while (!(stop && *stop)) {
		enum input_key key;
		int rows = ui_text(ui, &(struct ui_text_page){.title = page->title, .header = count ? page->header : NULL,
			.lines = count ? (const char *const *)page->lines : nothing, .count = count ? count : 1,
			.first = *first, .align = page->align, .footer = page->footer});
		int last = count > rows ? count - rows : 0;
		key = input_next(input, 1000);
		if (key == INPUT_UP)
			(*first)--;
		else if (key == INPUT_DOWN)
			(*first)++;
		else if (key == INPUT_LEFT)
			*first -= rows;
		else if (key == INPUT_RIGHT)
			*first += rows;
		else if (key != INPUT_NONE)
			return key;
		if (*first > last)
			*first = last;
		if (*first < 0)
			*first = 0;
	}
	return INPUT_NONE;
}

int list_move(enum input_key key, int selected, int count)
{
	int page = ui_menu_rows();
	if (count <= 0)
		return selected;
	if (key == INPUT_UP)
		return (selected + count - 1) % count;
	if (key == INPUT_DOWN)
		return (selected + 1) % count;
	if (key == INPUT_LEFT)
		return selected > page ? selected - page : 0;
	if (key == INPUT_RIGHT)
		return selected + page < count ? selected + page : count - 1;
	return selected;
}

int ask(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop, const char *title,
	const char *question, int yes)
{
	const char *items[2];
	char footer[96];
	int selected = yes ? 0 : 1;
	int answer = 0;
	ui_overlay(ui, 1);
	while (!(stop && *stop)) {
		enum input_key key;
		items[0] = _("Yes");
		items[1] = _("No");
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Confirm")});
		ui_menu(ui, title, question, items, 2, selected, footer);
		key = input_next(input, 1000);
		selected = list_move(key, selected, 2);
		if (key == INPUT_OK || key == INPUT_BACK) {
			answer = key == INPUT_OK && selected == 0;
			break;
		}
	}
	ui_overlay(ui, 0);
	return answer;
}

static long long milliseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

size_t text_break(const char *text, size_t *skip)
{
	size_t fit = ui_text_fit(text);
	size_t space = fit;
	*skip = 0;
	if (fit >= strlen(text))
		return fit;
	while (space > fit / 2 && text[space] != ' ')
		space--;
	if (space > fit / 2) {
		*skip = 1;
		return space;
	}
	return fit;
}

static void add_row(struct live_output *output, char *copy)
{
	if (!copy)
		return;
	if (output->count == LIVE_MAX_LINES) {
		free(output->lines[0]);
		memmove(output->lines, output->lines + 1, (LIVE_MAX_LINES - 1) * sizeof(*output->lines));
		output->count--;
	}
	output->lines[output->count++] = copy;
}

void live_output_add(struct live_output *output, const char *line)
{
	if (output->log) {
		fprintf(output->log, "%s\n", line);
		fflush(output->log);
	}
	if (!output->lines)
		output->lines = calloc(LIVE_MAX_LINES, sizeof(*output->lines));
	if (!output->lines)
		return;
	if (strchr(line, '\t')) {  /* Columns stay one row. */
		add_row(output, strdup(line));
		return;
	}
	do {  /* Broken like the crash log where it is wider than the view. */
		size_t skip;
		size_t length = text_break(line, &skip);
		add_row(output, strndup(line, length));
		line += length + skip;
	} while (*line);
}

void live_output_line(const char *line, void *opaque)
{
	struct live_output *output = opaque;
	live_output_add(output, line);
	if (milliseconds() - output->drawn >= 100)  /* The tick draws the rest. */
		live_output_tick(output);
}

void live_output_tick(void *opaque)
{
	const char *const waiting[] = {_("Please wait...")};
	struct live_output *output = opaque;
	int first;
	if (!output->rows)  /* The first drawing tells how many rows fit. */
		output->rows = ui_text(output->ui, &(struct ui_text_page){.title = output->title, .lines = waiting,
			.count = 1, .footer = output->footer});
	output->drawn = milliseconds();
	first = output->count > output->rows ? output->count - output->rows : 0;
	output->rows = ui_text(output->ui, &(struct ui_text_page){.title = output->title,
		.lines = output->count ? (const char *const *)output->lines : waiting,
		.count = output->count ? output->count : 1, .first = first, .footer = output->footer});
}

void live_output_view(const struct live_output *output, struct input_context *input, const volatile sig_atomic_t *stop,
	const char *note, int ok)
{
	char footer[128];
	char title[768];
	int first = output->count;  /* The end, text_view stops at the last page. */
	enum input_key key;
	snprintf(title, sizeof(title), "%s\n%s%s", output->title ? output->title : "", ok ? UI_GREEN : UI_RED, note);
	snprintf(footer, sizeof(footer), "ARROWS: %s   OK: %s", _("Scroll"), _("Menu"));
	do
		key = text_view(output->ui, input, stop, &(struct text_page){.title = title, .lines = output->lines,
			.count = output->count, .first = &first, .empty = _("Please wait..."), .footer = footer});
	while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_NONE);
}

void live_output_free(struct live_output *output)
{
	for (int i = 0; i < output->count; ++i)
		free(output->lines[i]);
	free(output->lines);
	output->lines = NULL;
	output->count = 0;
}

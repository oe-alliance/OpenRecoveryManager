#ifndef RECOVERY_VIEWER_H
#define RECOVERY_VIEWER_H

#include <signal.h>
#include <stdio.h>

#include "input.h"
#include "ui.h"

/* Lines like ui_text from *first on, empty shows when there are none. */
struct text_page {
	const char *title;
	const char *header;
	char *const *lines;
	int count;
	int *first;
	const char *align;
	const char *empty;
	const char *footer;
};

/* Scrolls the lines of page until a key that is no scroll key, which it returns. */
enum input_key text_view(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct text_page *page);

/* UP and DOWN move one item and wrap, LEFT and RIGHT a page of the menu drawn last and stop at
 * its ends; other keys keep selected. */
int list_move(enum input_key key, int selected, int count);

/* Yes or No in a dialog over the screen, yes chosen at first or no; BACK is no. Returns 1 for yes. */
int ask(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop, const char *title,
	const char *question, int yes);

/* The output of a command as it comes, the newest lines at the bottom. The
 * callbacks fit process_run_with_updates(). */
/* The bytes of text on its first line in the text view, broken at a space if one is near; skip is 1 for
 * that space. All of it for a text that fits. */
size_t text_break(const char *text, size_t *skip);

struct live_output {
	const struct ui_context *ui;
	const char *title;
	const char *footer;
	FILE *log;  /* A copy of every line, may be NULL. */
	char **lines;
	int count;
	int rows;
	long long drawn;  /* When, in ms, so a flood of lines is not drawn one by one. */
};

void live_output_line(const char *line, void *opaque);
/* Like live_output_line without drawing, for an output that is already complete. */
void live_output_add(struct live_output *output, const char *line);
void live_output_tick(void *opaque);
/* The finished output to scroll through until OK or BACK, the note below its title in green or red. */
void live_output_view(const struct live_output *output, struct input_context *input, const volatile sig_atomic_t *stop,
	const char *note, int ok);
void live_output_free(struct live_output *output);

#endif

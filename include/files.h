#ifndef RECOVERY_FILES_H
#define RECOVERY_FILES_H

#include <signal.h>

#include "input.h"
#include "ui.h"

/* A small file manager for medium, e.g. "/media/usb": folders and files by the space they take, OK opens a
 * folder, RED deletes after a question. need is the space that is wanted, 0 for none. */
void files_free_space(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop,
	const char *medium, long long need);

/* "1.2 GB", "412 MB", "12 KB" or "0 B". */
void files_size_text(long long bytes, char *text, size_t size);

#endif

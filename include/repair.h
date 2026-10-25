#ifndef RECOVERY_REPAIR_H
#define RECOVERY_REPAIR_H

#include <signal.h>

#include "input.h"
#include "ui.h"

/* Python files in the folders of enigma2 that no installed package has, put there by a skin or an
 * add-on; a .py of them is loaded instead of the .pyc of enigma2 next to it, which python may also
 * overwrite. changed are the python files of enigma2 itself that were written after it was installed. */
struct foreign {
	char **paths;
	int count;
	char **changed;
	int changed_count;
};

void repair_find_foreign(struct foreign *f);
/* 1 for a foreign file, 2 for a changed one; path may be a .py of the traceback whose .pyc changed. */
int repair_is_foreign(const struct foreign *f, const char *path);
void repair_free_foreign(struct foreign *f);

/* Moves the foreign files aside and installs enigma2 again from the feed, after a question. */
void repair_system_files(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct foreign *f);

#endif

#define _GNU_SOURCE

#include "console.h"

#include "boxinfo.h"
#include "i18n.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define QUESTION_MARK " [y/N] ("

int console_start(struct console *c, const char *path, char *const argv[])
{
	struct winsize size = {0, 200, 0, 0};  /* Wide, so no line is wrapped. */
	c->master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (c->master < 0 || grantpt(c->master) != 0 || unlockpt(c->master) != 0)
		return 0;
	c->pid = fork();
	if (c->pid < 0)
		return 0;
	if (c->pid == 0) {
		int slave;
		setsid();
		slave = open(ptsname(c->master), O_RDWR);
		if (slave < 0)
			_exit(127);
		ioctl(slave, TIOCSCTTY, 0);
		ioctl(slave, TIOCSWINSZ, &size);
		dup2(slave, STDIN_FILENO);
		dup2(slave, STDOUT_FILENO);
		dup2(slave, STDERR_FILENO);
		if (slave > STDERR_FILENO)
			close(slave);
		setenv("TERM", "dumb", 1);
		execv(path, argv);
		_exit(127);
	}
	fcntl(c->master, F_SETFL, fcntl(c->master, F_GETFL) | O_NONBLOCK);
	return 1;
}

int console_stopped(const struct console *c)
{
	return c->stop && *c->stop;
}

void console_answer(const struct console *c, int yes)
{
	if (write(c->master, yes ? "y\n" : "n\n", 2) < 0) {
		/* The program ended meanwhile. */
	}
}

void console_interrupt(const struct console *c)
{
	if (write(c->master, "\x03", 1) < 0) {
		/* The program ended meanwhile. */
	}
}

void console_close(struct console *c)
{
	if (c->master >= 0)
		close(c->master);
	c->master = -1;
}

int console_ask(const struct console *c, const char *title, const char *question, int seconds)
{
	const char *items[2];
	char heading[96];
	char footer[128];
	struct timespec now;
	long long deadline;
	int selected = 1;
	int yes = 0;
	int answered = 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + seconds * 1000LL;
	items[0] = _("Yes");
	items[1] = _("No");
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = _("Select"), .ok = _("Answer")});
	ui_overlay(c->ui, 1);  /* The screen below comes back after the answer. */
	while (!answered && !console_stopped(c)) {
		long long remaining;
		enum input_key key;
		clock_gettime(CLOCK_MONOTONIC, &now);
		remaining = deadline - ((long long)now.tv_sec * 1000 + now.tv_nsec / 1000000);
		if (remaining <= 0)
			break;  /* The program takes no answer in time as no. */
		snprintf(heading, sizeof(heading), "%s (%d)", title, (int)((remaining + 999) / 1000));
		ui_menu(c->ui, heading, question, items, 2, selected, footer);
		key = input_next(c->input, (int)((remaining - 1) % 1000) + 1);  /* Until the next second. */
		if (key == INPUT_UP || key == INPUT_DOWN || key == INPUT_LEFT || key == INPUT_RIGHT)
			selected = 1 - selected;
		else if (key == INPUT_OK || (key == INPUT_BACK && input_jumping())) {  /* Leaving it by a digit is no. */
			yes = key == INPUT_OK && selected == 0;
			console_answer(c, yes);
			answered = 1;
		}
	}
	ui_overlay(c->ui, 0);
	return yes;
}

static void encode_qrcode(struct console *c, const char *text)
{
	uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
	c->has_qrcode = qrcodegen_encodeText(text, temp, c->qrcode,
		qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
		qrcodegen_Mask_AUTO, true);
}

static void handle_line(struct console *c, char *line)
{
	char *link;
	text_cut(line, "\r");
	if (!strncmp(line, "QR: ", 4)) {  /* Before or after the link, it wins over the link. */
		link = line + 4 + strspn(line + 4, " \t");
		text_cut(link, " \t");
		if (*link) {
			encode_qrcode(c, link);
			c->qrcode_own = c->has_qrcode;
		}
		return;
	}
	link = strstr(line, "https://");
	if (!c->link[0] && link) {
		text_cut(link, " \t");
		snprintf(c->link, sizeof(c->link), "%s", link);
		if (!c->qrcode_own)
			encode_qrcode(c, c->link);
		return;
	}
	if (c->line)
		c->line(c, line);
}

/* Complete lines go to the owner, an unfinished one may be a question. */
static void handle_output(struct console *c)
{
	char *start = c->buffer;
	char *newline;
	const char *mark;
	if (c->used >= sizeof(c->buffer))
		c->used = sizeof(c->buffer) - 1;
	c->buffer[c->used] = '\0';  /* NOSONAR used is limited to the buffer just above */
	while ((newline = strchr(start, '\n'))) {
		*newline = '\0';
		handle_line(c, start);
		start = newline + 1;
	}
	mark = strstr(start, QUESTION_MARK);
	if (mark && strstr(mark, " s)")) {
		char question[512];
		int seconds = atoi(mark + strlen(QUESTION_MARK));
		text_cut(start, "\r");
		snprintf(question, sizeof(question), "%.*s", (int)(mark - start), start);
		c->used = 0;
		if (c->question)
			c->question(c, question, seconds > 0 ? seconds : 30);
		return;
	}
	c->used = strlen(start);
	memmove(c->buffer, start, c->used);
}

static void read_output(struct console *c)
{
	ssize_t length;
	while ((length = read(c->master, c->buffer + c->used,
		sizeof(c->buffer) - 1 - c->used)) > 0) {
		c->used += (size_t)length;
		handle_output(c);
		if (c->used >= sizeof(c->buffer) - 1)
			c->used = 0;  /* An endless line without a question. */
	}
}

int console_poll(struct console *c, int timeout_ms)
{
	fd_set read_set;
	struct timeval timeout = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	if (waitpid(c->pid, NULL, WNOHANG) == c->pid) {
		read_output(c);
		return 1;
	}
	FD_ZERO(&read_set);
	FD_SET(c->master, &read_set);
	if (select(c->master + 1, &read_set, NULL, NULL, &timeout) > 0)
		read_output(c);
	return 0;
}

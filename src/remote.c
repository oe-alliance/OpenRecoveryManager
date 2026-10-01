#define _GNU_SOURCE

#include "remote.h"

#include "console.h"
#include "i18n.h"
#include "process.h"
#include "viewer.h"

#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE _("Remote Support")
#define SESSION_DIR "/tmp/remotesupport"
#define SSHX_PID_FILE SESSION_DIR "/sshx.pid"
/* Installed into RAM from the feed when the image has no remote support. */
#define PACKAGE "enigma2-plugin-systemplugins-remotesupport"
#define RAM_ROOT "/tmp/orm-remotesupport"
#define RAM_SCRIPT RAM_ROOT "/usr/lib/enigma2/python/Plugins/SystemPlugins/RemoteSupport/RemoteSupport.pyc"

/* remotesupport keeps running in the background while the menu is used, its
 * questions appear over every screen. enigma2 takes the session over when it
 * starts, remotesupport ends with ORM. */
struct session {
	struct console console;
	int ending;  /* Ctrl+C was sent, the next question is whether to end: yes. */
	int polling;
	int in_ram;
	char status[256];
};

static struct session *background;
static int unavailable;  /* BLUE is left out of the menu then. */

int remote_support_available(void)
{
	return !unavailable;
}

static void handle_line(const struct console *console, const char *line)
{
	struct session *s = console->data;
	/* The QR code of the command line consists of block characters. */
	if (line[0] && !strstr(line, "\xe2\x96") && !strchr(line, '#'))
		snprintf(s->status, sizeof(s->status), "%.255s", line);
}

static void handle_question(const struct console *console, const char *question, int seconds)
{
	const struct session *s = console->data;
	if (s->ending)
		console_answer(console, 1);
	else
		console_ask(console, TITLE, question, seconds);
}

static void end_background(void)
{
	input_set_idle(NULL);
	console_close(&background->console);
	free(background);
	background = NULL;
}

/* In every screen through input_next: questions of the session, its end. */
static void poll_background(void)
{
	if (!background || background->polling)
		return;
	background->polling = 1;  /* console_ask waits for keys itself. */
	if (console_poll(&background->console, 0)) {
		end_background();
		return;
	}
	background->polling = 0;
}

static int confirm_end(const struct session *s)
{
	return ask(s->console.ui, s->console.input, s->console.stop, TITLE,
		_("End the Remote Support session? The supporter can then no longer work on the receiver."), 1);
}

static int terminals(void);

static void show_screen(const struct session *s)
{
	const struct console *c = &s->console;
	char body[1024];
	char summary[256];
	char footer[160];
	ui_busy(c->ui, !c->link[0]);  /* The start of the session takes a while. */
	if (!c->link[0]) {
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.red = _("Cancel")});
		ui_progress(c->ui, TITLE, _("Starting the Remote Support session."), 50,
			_("Connecting to the support server, this takes a few seconds."), footer);
		return;
	}
	remote_support_summary(summary, sizeof(summary));
	/* The texts of the plugin; its QR code of a "QR:" line opens a page to share the link from the phone. */
	snprintf(body, sizeof(body), "%s %s\n\n%s\n\n%s%s%s",
		c->qrcode_own ? _("Scan the QR code with your phone and send the link to your supporter from there.") :
		_("Send the link below to your supporter or let them scan the QR code."),
		terminals() ? _("Approved users can open terminals. Everybody who joins later has to be approved as well.") :
		_("When somebody joins the session, you have to approve the access with the remote control."),
		s->in_ram ? _("The session keeps running in the menu and ends when Enigma2 starts.") :
		_("The session keeps running in the menu and after Enigma2 starts."),
		summary, s->ending ? "\n\n" : "", s->ending ? _("Ending the session...") : "");
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = _("End session"), .back = _("Menu")});
	ui_remote_session(c->ui, &(struct ui_remote_page){.title = TITLE, .body = body, .link = c->link,
		.warning = _("Do not post the link or a screenshot of it in public forums: everybody who has it can "
		"try to join and sees the terminals until you deny the access."),
		.qrcode = c->has_qrcode ? c->qrcode : NULL,
		.qr_hint = c->qrcode_own ? _("Scan with your phone to send the link to your supporter") :
		_("Scan to open the support session"),
		.footer = footer});
}

static void show_end(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const char *message)
{
	enum input_key key;
	ui_busy(ui, 0);
	ui_error(ui, TITLE, message);
	do {
		key = input_next(input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
}

int remote_support_running(void)
{
	FILE *file = fopen(SSHX_PID_FILE, "r");
	int pid = 0;
	if (!file)
		return 0;
	if (fscanf(file, "%d", &pid) != 1)
		pid = 0;
	fclose(file);
	return pid > 0 && kill(pid, 0) == 0;
}

/* The participants the session writes to "connected", uid, name and whether the name is final per line. */
static int participants(char *names, size_t size)
{
	char line[256];
	int count = 0;
	FILE *file = fopen(SESSION_DIR "/connected", "r");
	if (names && size)
		names[0] = '\0';
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file)) {
		char *name = strchr(line, '\t');
		const char *end = name ? strchr(name + 1, '\t') : NULL;
		size_t used = names ? strlen(names) : 0;
		if (!end)
			continue;
		count++;
		if (names && used + 3 < size)
			snprintf(names + used, size - used, "%s%.*s", used ? ", " : "", (int)(end - name - 1), name + 1);
	}
	fclose(file);
	return count;
}

/* Open terminals: the shell still runs and waits for no approval, like "remotesupport status". */
static int terminals(void)
{
	DIR *dir = opendir(SESSION_DIR);
	const struct dirent *entry;
	int count = 0;
	if (!dir)
		return 0;
	while ((entry = readdir(dir))) {
		char path[300];
		int pid;
		char tail[8];
		if (sscanf(entry->d_name, "term-%d%7s", &pid, tail) != 2 || strcmp(tail, ".log") != 0)
			continue;
		snprintf(path, sizeof(path), SESSION_DIR "/term-%d.request", pid);
		if (kill(pid, 0) == 0 && access(path, F_OK) != 0)
			count++;
	}
	closedir(dir);
	return count;
}

void remote_support_summary(char *text, size_t size)
{
	char names[200];
	int people = participants(names, sizeof(names));
	snprintf(text, size, _("Users: %s\nTerminals: %d"), people ? names : "0", terminals());
}

int remote_support_counts(int *users, int *open)
{
	char names[200];
	if (!remote_support_running() || (background && !background->console.link[0]))  /* Not while starting. */
		return 0;
	*users = participants(names, sizeof(names));
	*open = terminals();
	return 1;
}

/* opkg with a copy of the status and the lists in RAM, so nothing is written to the flash. */
static int write_opkg_conf(void)
{
	FILE *conf;
	FILE *status;
	glob_t feeds;
	char line[512];
	mkdir(RAM_ROOT, 0755);
	mkdir(RAM_ROOT "/lists", 0755);
	if (!(conf = fopen(RAM_ROOT "/opkg.conf", "w")))
		return 0;
	if (glob("/etc/opkg/*.conf", 0, NULL, &feeds) == 0) {
		for (size_t i = 0; i < feeds.gl_pathc; ++i) {
			FILE *feed;
			if (!strcmp(feeds.gl_pathv[i], "/etc/opkg/opkg.conf"))
				continue;
			feed = fopen(feeds.gl_pathv[i], "r");
			if (!feed)
				continue;
			while (fgets(line, sizeof(line), feed))
				fputs(line, conf);
			fclose(feed);
		}
		globfree(&feeds);
	}
	fprintf(conf, "\ndest root /\ndest ram " RAM_ROOT "\noption lists_dir " RAM_ROOT "/lists\n"
		"option info_dir /var/lib/opkg/info\noption status_file " RAM_ROOT "/root-status\n");
	fclose(conf);
	if (!(status = fopen(RAM_ROOT "/root-status", "w")))
		return 0;
	if ((conf = fopen("/var/lib/opkg/status", "r"))) {
		size_t length;
		while ((length = fread(line, 1, sizeof(line), conf)) > 0)
			fwrite(line, 1, length, status);  /* NOSONAR a copy of the opkg status */
		fclose(conf);
	}
	return fclose(status) == 0;
}

static int install_into_ram(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	char *const update[] = {"opkg", "-f", RAM_ROOT "/opkg.conf", "update", NULL};
	char *const install[] = {"opkg", "-f", RAM_ROOT "/opkg.conf", "-d", "ram", "install", PACKAGE, NULL};
	char title[128];
	char footer[160];
	struct live_output output = {.ui = ui, .title = title,
		.footer = _("Not installed, it goes into RAM until the next restart, the flash stays as it is. Please wait...")};
	int result = write_opkg_conf() ? 0 : -1;
	snprintf(title, sizeof(title), "%s - %s", TITLE, _("Installing into RAM"));
	if (result == 0)
		result = process_run_with_updates(update, NULL, live_output_line, live_output_tick, 100, &output);
	if (result != 0) {  /* Like enigma2 an unavailable extra feed does not matter when openatv-all is there. */
		for (int i = 0; i < output.count; ++i)
			if (strstr(output.lines[i], "Updated source") && strstr(output.lines[i], "openatv-all"))
				result = 0;
	}
	if (result == 0)
		result = process_run_with_updates(install, NULL, live_output_line, live_output_tick, 100, &output);
	if (result == 0 && access(RAM_SCRIPT, R_OK) != 0)
		result = -1;
	if (result != 0) {
		enum input_key key;
		snprintf(footer, sizeof(footer), "%s   OK: %s", _("Remote Support could not be installed."), _("Menu"));
		output.footer = footer;
		live_output_tick(&output);
		do {
			key = input_next(input, 1000);
		} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED && !(stop && *stop));
	}
	live_output_free(&output);
	return result == 0;
}

/* Runs the plugin in RAM through env, with sshx and the Python packages of RAM first. */
static int start_in_ram(struct console *console)
{
	static char path[512];
	static char python_path[256];
	static char *argv[] = {"env", path, python_path, "/usr/bin/python3", RAM_SCRIPT, "console", "start", NULL};
	glob_t found;
	const char *old = getenv("PATH");
	snprintf(path, sizeof(path), "PATH=" RAM_ROOT "/usr/bin:%.400s", old ? old : "/usr/bin:/bin");
	snprintf(python_path, sizeof(python_path), "PYTHONPATH=");
	if (glob(RAM_ROOT "/usr/lib/python3*/site-packages", 0, NULL, &found) == 0) {
		snprintf(python_path, sizeof(python_path), "PYTHONPATH=%.200s", found.gl_pathv[0]);
		globfree(&found);
	}
	return console_start(console, "/usr/bin/env", argv);
}

static struct session *start_session(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, int in_ram)
{
	static char *const argv[] = {"remotesupport", "start", NULL};
	struct session *s;
	if (in_ram && access(RAM_SCRIPT, R_OK) != 0 && !install_into_ram(ui, input, stop))
		return NULL;
	if (!(s = calloc(1, sizeof(*s))))
		return NULL;
	s->in_ram = in_ram;
	s->console = (struct console){.ui = ui, .input = input, .stop = stop, .master = -1,
		.line = handle_line, .question = handle_question, .data = s};
	if (!(in_ram ? start_in_ram(&s->console) : console_start(&s->console, REMOTESUPPORT, argv))) {
		console_close(&s->console);
		free(s);
		return NULL;
	}
	return s;
}

static void start_failed(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	unavailable = 1;
	show_end(ui, input, stop, _("Remote Support cannot be started."));
}

/* 1: end the session, -1: back to the menu, 0: nothing. */
static int screen_action(const struct session *s, enum input_key key)
{
	if (console_stopped(&s->console) || ((key == INPUT_OK) && confirm_end(s)))
		return 1;
	if (key == INPUT_BACK && s->console.link[0])
		return -1;
	return key == INPUT_RED || key == INPUT_BACK;  /* Cancels the start. */
}

/* The session until it ends; 0 when BACK returns to the menu first. */
static int run_screen(struct session *s, struct input_context *input)
{
	input_set_idle(NULL);  /* This screen reads the session itself. */
	while (!console_poll(&s->console, 300)) {
		enum input_key key;
		int action;
		show_screen(s);
		key = input_next(input, 200);
		if (s->ending)
			continue;
		action = screen_action(s, key);
		if (action < 0) {
			input_set_idle(poll_background);
			return 0;
		}
		if (action) {
			s->ending = 1;
			console_interrupt(&s->console);
		}
	}
	return 1;
}

/* The installed plugin first, then its copy in RAM, installed from the feed when missing. One
 * that ends without a link did not start; when none starts, BLUE is left out. */
void remote_support(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop)
{
	struct session *s = background;
	int in_ram = access(REMOTESUPPORT, X_OK) != 0;
	if (!s) {
		if (!remote_support_running() && !ask(ui, input, stop, TITLE, _("Start Remote Support? A supporter can then "
			"work on the receiver from the internet."), 0))  /* A running session is only taken over. */
			return;
		s = start_session(ui, input, stop, in_ram);
		if (!s && !in_ram)
			s = start_session(ui, input, stop, 1);
		if (!s) {
			start_failed(ui, input, stop);
			return;
		}
		background = s;
	}
	for (;;) {
		if (!run_screen(s, input))
			return;
		if (s->ending || s->console.link[0])
			break;
		in_ram = s->in_ram;
		end_background();
		s = in_ram ? NULL : start_session(ui, input, stop, 1);
		if (!s) {
			start_failed(ui, input, stop);
			return;
		}
		background = s;
	}
	ui_busy(ui, 0);
	if (!s->ending)
		show_end(ui, input, stop, s->status[0] ? s->status : _("The Remote Support session ended."));
	end_background();
}

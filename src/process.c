#define _GNU_SOURCE

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void (*idle_call)(void);
static unsigned int idle_ms;
static uint64_t idle_next;

static uint64_t current_milliseconds(void)
{
	struct timeval now;
	if (gettimeofday(&now, NULL) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000ULL +
		(uint64_t)now.tv_usec / 1000ULL;
}

void process_set_idle(void (*idle)(void), unsigned int ms)
{
	idle_call = idle;
	idle_ms = ms;
}

void process_idle(void)
{
	if (!idle_call || current_milliseconds() < idle_next)
		return;
	idle_call();
	idle_next = current_milliseconds() + idle_ms;  /* After it, a redraw can take longer than ms. */
}

int process_select(int maximum, fd_set *read_set, int timeout_ms)
{
	uint64_t end = current_milliseconds() + (uint64_t)(timeout_ms < 0 ? 0 : timeout_ms);
	for (;;) {
		fd_set ready = *read_set;
		struct timeval timeout;
		uint64_t now;
		uint64_t wait;
		int result;
		process_idle();
		now = current_milliseconds();
		if (timeout_ms < 0)
			wait = UINT64_MAX;
		else
			wait = end > now ? end - now : 0;
		if (idle_call && wait > idle_ms)
			wait = idle_next > now ? idle_next - now : 0;
		timeout.tv_sec = (time_t)(wait / 1000ULL);
		timeout.tv_usec = (suseconds_t)((wait % 1000ULL) * 1000ULL);
		result = select(maximum + 1, &ready, NULL, NULL, wait == UINT64_MAX ? NULL : &timeout);
		if (result != 0 || (timeout_ms >= 0 && current_milliseconds() >= end)) {
			*read_set = ready;
			return result;
		}
	}
}

static void emit_lines(char *pending, size_t *pending_length,
	const char *data, size_t length, process_line_cb callback, void *opaque)
{
	for (size_t i = 0; i < length; ++i) {
		char ch = data[i];
		if (ch == '\r')
			continue;
		if (ch == '\n' || *pending_length + 1 >= 512) {
			pending[*pending_length] = '\0';
			if (callback)  /* Empty lines too, the output as it comes. */
				callback(pending, opaque);
			*pending_length = 0;
			continue;
		}
		pending[(*pending_length)++] = ch;
	}
}

static volatile int cancel_requested;

void process_cancel(void)
{
	cancel_requested = 1;
}

struct run_state {
	pid_t child;
	int fd;
	int status;
	int child_reaped;
	int wait_error;
	int cancelled;
	process_line_cb callback;
	process_tick_cb tick;
	unsigned int tick_ms;
	uint64_t next_tick;
	void *opaque;
	char pending[512];
	size_t pending_length;
};

/* A terminal, so programs like opkg write every line at once, not a full buffer at the end. */
static int open_terminal(int output_pipe[2])
{
	struct winsize size = {0, 200, 0, 0};  /* Wide, so no line is wrapped. */
	output_pipe[0] = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (output_pipe[0] < 0)
		return -1;
	if (grantpt(output_pipe[0]) == 0 && unlockpt(output_pipe[0]) == 0)
		output_pipe[1] = open(ptsname(output_pipe[0]), O_RDWR | O_NOCTTY);
	if (output_pipe[1] < 0) {
		close(output_pipe[0]);
		return -1;
	}
	ioctl(output_pipe[1], TIOCSWINSZ, &size);
	return 0;
}

static _Noreturn void run_child(char *const argv[], const char *stdin_text,
	const int output_pipe[2], const int input_pipe[2])
{
	int null_fd;
	setpgid(0, 0);  /* Its own group, so a cancel ends everything it started. */
	dup2(output_pipe[1], STDOUT_FILENO);
	dup2(output_pipe[1], STDERR_FILENO);
	close(output_pipe[0]);
	close(output_pipe[1]);
	if (stdin_text) {
		dup2(input_pipe[0], STDIN_FILENO);
		close(input_pipe[0]);
		close(input_pipe[1]);
	} else {
		null_fd = open("/dev/null", O_RDONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			close(null_fd);
		}
	}
	execvp(argv[0], argv);  /* NOSONAR only the fixed commands of ORM */
	dprintf(STDERR_FILENO, "Cannot execute %s: %s\n", argv[0],
		strerror(errno));
	_exit(127);
}

static void write_input(int fd, const char *stdin_text)
{
	size_t length = strlen(stdin_text);
	size_t written = 0;
	while (written < length) {
		ssize_t result = write(fd, stdin_text + written, length - written);  /* NOSONAR the input ORM gives its own command */
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		written += (size_t)result;
	}
}

/* Calls tick when it is due; returns the current time. */
static uint64_t tick_when_due(struct run_state *state)
{
	uint64_t now = current_milliseconds();
	if (now >= state->next_tick) {
		state->tick(state->opaque);
		state->next_tick = now + state->tick_ms;
	}
	return now;
}

/* 0 when waiting for the child failed. */
static int check_child(struct run_state *state)
{
	if (!state->child_reaped) {
		pid_t waited = waitpid(state->child, &state->status, WNOHANG);
		if (waited == state->child)
			state->child_reaped = 1;
		else if (waited < 0 && errno != EINTR) {
			state->wait_error = 1;
			return 0;
		}
	}
	if (cancel_requested && !state->cancelled && !state->child_reaped) {
		state->cancelled = 1;
		kill(-state->child, SIGTERM);
	}
	return 1;
}

/* How long select waits for output, in milliseconds. */
static uint64_t wait_time(struct run_state *state)
{
	uint64_t remaining = 250;
	if (state->tick && state->tick_ms) {
		uint64_t now = tick_when_due(state);
		remaining = state->next_tick > now ? state->next_tick - now : 0;
		if (remaining > 250)
			remaining = 250;
	}
	if (idle_call) {
		uint64_t now = current_milliseconds();
		uint64_t until_idle = idle_next > now ? idle_next - now : 0;
		if (remaining > until_idle)
			remaining = until_idle;
	}
	if (state->child_reaped)
		remaining = 0;
	return remaining;
}

/* 0 when there is no more output. */
static int read_output(struct run_state *state)
{
	char buffer[256];
	ssize_t result = read(state->fd, buffer, sizeof(buffer));
	if (result > 0) {
		emit_lines(state->pending, &state->pending_length, buffer, (size_t)result,
			state->callback, state->opaque);
		if (state->tick && state->tick_ms)
			tick_when_due(state);
		return 1;
	}
	return result < 0 && errno == EINTR;
}

/* One round of the output loop; 0 when it ends. */
static int run_step(struct run_state *state)
{
	fd_set read_set;
	struct timeval timeout;
	uint64_t remaining;
	int ready;

	if (!check_child(state))
		return 0;
	process_idle();
	remaining = wait_time(state);
	FD_ZERO(&read_set);
	FD_SET(state->fd, &read_set);
	timeout.tv_sec = (time_t)(remaining / 1000ULL);
	timeout.tv_usec = (suseconds_t)((remaining % 1000ULL) * 1000ULL);
	ready = select(state->fd + 1, &read_set, NULL, NULL, &timeout);
	if (ready == 0)
		return !state->child_reaped;
	if (ready < 0)
		return errno == EINTR;
	return read_output(state);
}

static int finish_run(struct run_state *state)
{
	if (state->pending_length) {
		state->pending[state->pending_length] = '\0';
		if (state->callback)
			state->callback(state->pending, state->opaque);
	}
	if (!state->child_reaped) {
		while (waitpid(state->child, &state->status, 0) < 0) {
			if (errno != EINTR)
				return -1;
		}
	}
	if (state->wait_error)
		return -1;
	if (WIFEXITED(state->status))
		return WEXITSTATUS(state->status);
	if (WIFSIGNALED(state->status))
		return 128 + WTERMSIG(state->status);
	return -1;
}

static int process_run_internal(char *const argv[], const char *stdin_text,
	process_line_cb callback, process_tick_cb tick, unsigned int tick_ms,
	void *opaque)
{
	int output_pipe[2] = {-1, -1};
	int input_pipe[2] = {-1, -1};
	struct run_state state = {.callback = callback, .tick = tick, .tick_ms = tick_ms,
		.opaque = opaque};
	int running = 1;

	if (!argv || !argv[0]) {
		errno = EINVAL;
		return -1;
	}
	if (tick) {
		if (open_terminal(output_pipe) < 0)
			return -1;
	} else if (pipe(output_pipe) < 0)
		return -1;
	if (stdin_text && pipe(input_pipe) < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		return -1;
	}

	cancel_requested = 0;
	state.child = fork();
	if (state.child < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		if (input_pipe[0] >= 0) {
			close(input_pipe[0]);
			close(input_pipe[1]);
		}
		return -1;
	}
	if (state.child == 0)
		run_child(argv, stdin_text, output_pipe, input_pipe);

	setpgid(state.child, state.child);  /* Also here, a cancel may come before the child set it. */
	close(output_pipe[1]);
	if (stdin_text) {
		close(input_pipe[0]);
		write_input(input_pipe[1], stdin_text);
		close(input_pipe[1]);
	}
	if (tick && tick_ms)
		state.next_tick = current_milliseconds() + tick_ms;

	state.fd = output_pipe[0];
	while (running)
		running = run_step(&state);
	close(output_pipe[0]);
	return finish_run(&state);
}

int process_run(char *const argv[], const char *stdin_text,
	process_line_cb callback, void *opaque)
{
	return process_run_internal(argv, stdin_text, callback, NULL, 0, opaque);
}

int process_run_with_updates(char *const argv[], const char *stdin_text,
	process_line_cb callback, process_tick_cb tick, unsigned int tick_ms,
	void *opaque)
{
	return process_run_internal(argv, stdin_text, callback, tick, tick_ms,
		opaque);
}

struct capture_state {
	char *output;
	size_t output_size;
	size_t used;
};

static void capture_line(const char *line, void *opaque)
{
	struct capture_state *state = opaque;
	size_t available;
	size_t length;

	if (!state || state->used + 1 >= state->output_size)
		return;
	available = state->output_size - state->used - 1;
	length = strlen(line);
	if (length > available)
		length = available;
	memcpy(state->output + state->used, line, length);
	state->used += length;
	state->output[state->used] = '\0';
}

int process_capture(char *const argv[], char *output, size_t output_size)
{
	struct capture_state state;

	if (!output || output_size == 0) {
		errno = EINVAL;
		return -1;
	}
	output[0] = '\0';
	state.output = output;
	state.output_size = output_size;
	state.used = 0;
	return process_run(argv, NULL, capture_line, &state);
}

int process_find(const char *name, char *path, size_t path_size)
{
	static const char *const directories[] = {
		"/usr/sbin", "/usr/bin", "/sbin", "/bin", NULL
	};

	if (!name || !path || path_size == 0)
		return 0;
	if (strchr(name, '/')) {
		if (access(name, X_OK) == 0) {
			snprintf(path, path_size, "%s", name);
			return 1;
		}
		return 0;
	}
	for (int i = 0; directories[i]; ++i) {
		snprintf(path, path_size, "%s/%s", directories[i], name);
		if (access(path, X_OK) == 0)
			return 1;
	}
	path[0] = '\0';
	return 0;
}

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <termios.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "config.h"
#include "clipboard.h"
#include <fontconfig/fontconfig.h>
#include "draw.h"
#include "library.h"

#if COVER_WIDTH < 2 || COVER_WIDTH > 4096 || SPACING < 4 || SPACING > 1024
#error "Use a cover width of 2..4096 and spacing of 4..1024."
#endif

enum {
	CoverHeight = COVER_WIDTH * 3 / 2,
	TitleLines = 3,
	TileHeight = CoverHeight + 6 + TitleLines * (TITLE_FONT_SIZE + 3),
	Header = HEADER_FONT_SIZE + 20,
	Footer = STATUS_FONT_SIZE + 6
};
struct view {
	struct wl_surface *surface;
	struct xdg_surface *shell;
	struct xdg_toplevel *window;
	struct frame *frames[2];
	int width, height, configured, dirty;
};
struct frame {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t bytes;
	struct frame **slot;
};
static struct view main_view = { .width = 900, .height = 640 };
static struct wl_surface *focus;
static int selected, first_row, running = 1, active;
static char status[PATH_MAX + 128];
static enum { NoPrompt, SearchPrompt, NamePrompt, RenamePrompt, RemovePrompt, RepairPrompt } prompt_mode;
static int search_origin, found;
static int first_key_pending, help_page;
static char query[PATH_MAX], game_name[PATH_MAX], prompt_error[160];
static size_t input_cursor;
static int submit_after_paste;
extern char **environ;
static pid_t editor_pid;
static int editor_tty = -1;
static struct termios editor_termios;
static const int editor_signals[] = {SIGINT, SIGQUIT};
static struct sigaction editor_handlers[sizeof editor_signals / sizeof *editor_signals];
enum editor_action { AddConfig, EditConfig, ViewLog };
static enum editor_action editor_action;
static char editor_config[PATH_MAX];
static uint64_t previous_hash;
static char repair_message[PATH_MAX + 128];
static struct stat draft_revision;
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static uint32_t seat_name;
static struct wl_keyboard *keyboard;
static struct xdg_wm_base *wm;
static struct xkb_context *xkb;
static struct xkb_state *keys;
static int repeat_fd, repeat_rate = 25, repeat_delay = 600;
static uint32_t repeat_code;

static void show_view(void);

static void stop_repeat(void)
{
	if (!repeat_code) return;
	struct itimerspec off = {0};
	if (timerfd_settime(repeat_fd, 0, &off, NULL)) { perror("timerfd_settime"); exit(EXIT_FAILURE); }
	repeat_code = 0;
}

static void die(const char *message)
{
	fprintf(stderr, "ludus: %s\n", message);
	exit(EXIT_FAILURE);
}

static int columns(void)
{
	int n = (main_view.width - SPACING) / (COVER_WIDTH + SPACING);
	return n > 0 ? n : 1;
}

/* Prefer the character on the keycap, then fall back to its XKB name. */
static void key_label(xkb_keysym_t sym, char label[64])
{
	if (sym == XKB_KEY_Return) { strcpy(label, "Enter"); return; }
	if (xkb_keysym_to_utf8(sym, label, 64) > 1 && (unsigned char)label[0] > ' ' &&
	    (unsigned char)label[0] != 127) return;
	if (xkb_keysym_get_name(sym, label, 64) < 0) strcpy(label, "?");
}

static const struct {
	xkb_keysym_t key;
	const char *action;
} shortcuts[] = {
	{KEY_LEFT, "Move left"}, {KEY_DOWN, "Move down"}, {KEY_UP, "Move up"}, {KEY_RIGHT, "Move right"},
	{KEY_FIRST, "First game"}, {KEY_LAST, "Last game"}, {KEY_LAUNCH, "Play game"},
	{KEY_SEARCH, "Search titles"}, {KEY_NEXT_MATCH, "Next match"}, {KEY_PREV_MATCH, "Previous match"},
	{KEY_ADD, "Add config"}, {KEY_EDIT, "Edit config"}, {KEY_RENAME, "Rename config"},
	{KEY_REMOVE, "Trash config"}, {KEY_REFRESH, "Rescan library"}, {KEY_LOG, "Open launch log"},
	{KEY_HELP, "Show shortcuts"}, {KEY_QUIT, "Quit"}
};

static int help_rows(void)
{
	int rows = (main_view.height - Header - Footer - 8) / (STATUS_FONT_SIZE + 6);
	return rows > 0 ? rows : 1;
}

static int help_pages(void)
{
	int total = sizeof shortcuts / sizeof shortcuts[0];
	return (total + help_rows() - 1) / help_rows();
}

static void paint_help(struct canvas c)
{
	char label[64], binding[136];
	int rows = help_rows(), key_width = 0;
	if (help_page > help_pages()) help_page = help_pages();
	int first = (help_page - 1) * rows;
	fill(c, 0, 0, c.width, c.height, BACKGROUND);
	fill(c, 0, 0, c.width, Header, BAR_BACKGROUND);
	text(c, 8, HEADER_FONT_SIZE + 6, 0, c.width, HEADER_FONT_SIZE, TEXT_COLOR, "Shortcuts");
	for (int i = 0; i < (int)(sizeof shortcuts / sizeof shortcuts[0]); ++i) {
		key_label(shortcuts[i].key, label);
		if (shortcuts[i].key == KEY_FIRST)
			snprintf(binding, sizeof binding, "%s%s%s", label, strlen(label) > 1 ? " " : "", label);
		else strcpy(binding, label);
		int width = text_width(STATUS_FONT_SIZE, binding);
		if (width > key_width) key_width = width;
		if (i < first || i - first >= rows) continue;
		int y = Header + 4 + STATUS_FONT_SIZE + (i - first) * (STATUS_FONT_SIZE + 6);
		text(c, 8, y, 0, c.width, STATUS_FONT_SIZE, TEXT_COLOR, binding);
	}
	int x = 8 + key_width + 16;
	for (int i = first; i < (int)(sizeof shortcuts / sizeof shortcuts[0]) && i - first < rows; ++i) {
		int y = Header + 4 + STATUS_FONT_SIZE + (i - first) * (STATUS_FONT_SIZE + 6);
		text(c, x, y, x, c.width, STATUS_FONT_SIZE, TEXT_COLOR, shortcuts[i].action);
	}
	int bar = c.height - Footer;
	fill(c, 0, bar, c.width, Footer, BAR_BACKGROUND);
	const char *hint = help_pages() > 1 ? "Esc close   Space next page" : "Esc close";
	text(c, 6, bar + STATUS_FONT_SIZE + 1, 0, c.width, STATUS_FONT_SIZE, TEXT_COLOR, hint);
}

static void paint(struct canvas c)
{
	if (help_page) { paint_help(c); return; }
	int i, y, cols = columns(), width = c.width, height = c.height;
	int rows = (height - Header - Footer - SPACING) / (TileHeight + SPACING);
	int bar = height > Footer ? height - Footer : 0;
	int right = width - 6;
	int status_x = right - text_width(STATUS_FONT_SIZE, status);
	int split = status_x < width / 3 ? width / 3 : status_x;
	if (rows < 1) rows = 1;
	if (first_row > selected / cols) first_row = selected / cols;
	if (first_row <= selected / cols - rows) first_row = selected / cols - rows + 1;
	fill(c, 0, 0, width, height, BACKGROUND);
	fill(c, 0, 0, width, Header, BAR_BACKGROUND);
	text(c, SPACING, HEADER_FONT_SIZE + 6, 0, width, HEADER_FONT_SIZE, TEXT_COLOR, "ludus");
	char help_key[64], hint[72];
	key_label(KEY_HELP, help_key);
	snprintf(hint, sizeof hint, "%s Help", help_key);
	int hint_x = right - text_width(STATUS_FONT_SIZE, hint);
	if (prompt_mode == NoPrompt && hint_x > SPACING + text_width(HEADER_FONT_SIZE, "ludus") + 8)
		text(c, hint_x, (Header + STATUS_FONT_SIZE) / 2, 0, right, STATUS_FONT_SIZE, TEXT_COLOR, hint);
	static int cached_begin, cached_end;
	int begin = first_row * cols;
	int end = begin;
	for (i = begin; i < count; ++i) {
		int left = SPACING + (i % cols) * (COVER_WIDTH + SPACING);
		int top = Header + SPACING + (i / cols - first_row) * (TileHeight + SPACING);
		int visible = width - left;
		if (top >= bar) break;
		end = i + 1;
		if (visible <= 0) continue;
		if (visible > COVER_WIDTH) visible = COVER_WIDTH;
		if (i == selected)
			fill(c, left - 2, top - 2, COVER_WIDTH + 4, CoverHeight + 4, ACCENT_COLOR);
		if (!games[i].tried) load_cover(&games[i]);
		wrapped_title(c, left, top + CoverHeight + 6 + TITLE_FONT_SIZE, COVER_WIDTH, TITLE_FONT_SIZE,
		              TitleLines, TEXT_COLOR, games[i].name);
		if (!games[i].thumbnail) {
			fill(c, left, top, COVER_WIDTH, CoverHeight, EMPTY_COVER_COLOR);
			continue;
		}
		for (y = 0; y < CoverHeight && top + y < bar; ++y)
			memcpy(c.pixels + (size_t)(top + y) * width + left,
			       games[i].thumbnail + y * COVER_WIDTH, visible * sizeof(*c.pixels));
	}
	for (i = cached_begin; i < cached_end && i < count; ++i) {
		if (i >= begin && i < end) continue;
		free(games[i].thumbnail);
		games[i].thumbnail = NULL;
		games[i].tried = 0;
	}
	cached_begin = begin;
	cached_end = end;
	fill(c, 0, bar, width, Footer, BAR_BACKGROUND);
	if (prompt_mode == RepairPrompt) {
		text(c, 6, bar + STATUS_FONT_SIZE + 1, 6, right, STATUS_FONT_SIZE, TEXT_COLOR, repair_message);
		return;
	}
	if (prompt_mode == RemovePrompt) {
		char confirm[PATH_MAX + 32];
		snprintf(confirm, sizeof confirm, "Enter to remove %s", games[selected].name);
		text(c, 6, bar + STATUS_FONT_SIZE + 1, 6, right, STATUS_FONT_SIZE, TEXT_COLOR, confirm);
		return;
	}
	if (prompt_mode != NoPrompt) {
		int search = prompt_mode == SearchPrompt;
		const char *label = search ? "/" : prompt_mode == RenamePrompt ? "Rename to: " : "Game name: ";
		const char *value = search ? query : game_name;
		const char *message = search ? (found ? games[selected].name : "No match") : prompt_error;
		int edge = search ? width / 2 : right;
		if (!search && *message) edge -= text_width(STATUS_FONT_SIZE, message) + 4;
		if (edge < width / 3) edge = width / 3;
		char prompt[PATH_MAX + sizeof "Game name: "];
		snprintf(prompt, sizeof prompt, "%s%s", label, value);
		size_t caret_pos = strlen(label) + input_cursor;
		char saved = prompt[caret_pos];
		prompt[caret_pos] = 0;
		int caret = text_width(STATUS_FONT_SIZE, prompt);
		prompt[caret_pos] = saved;
		int start = caret + 8 < edge ? 6 : edge - caret - 2;
		text(c, start, bar + STATUS_FONT_SIZE + 1, 6, edge, STATUS_FONT_SIZE, TEXT_COLOR, prompt);
		fill(c, start + caret, bar + 3, 1, STATUS_FONT_SIZE, ACCENT_COLOR);
		if (*message) text(c, edge + 4, bar + STATUS_FONT_SIZE + 1, edge + 4, right, STATUS_FONT_SIZE, TEXT_COLOR, message);
		return;
	}
	text(c, 6, bar + STATUS_FONT_SIZE + 1, 0, split - 4, STATUS_FONT_SIZE, TEXT_COLOR,
	     count ? games[selected].name : "Press + to add a game");
	if (*status)
		text(c, status_x, bar + STATUS_FONT_SIZE + 1, split, right, STATUS_FONT_SIZE, TEXT_COLOR, status);

}

static void released(void *data, struct wl_buffer *buffer)
{
	struct frame *frame = data;
	wl_buffer_destroy(buffer);
	munmap(frame->pixels, frame->bytes);
	*frame->slot = NULL;
	free(frame);
}
static const struct wl_buffer_listener buffer_events = { .release = released };

static void destroy_view(struct view *v)
{
	xdg_toplevel_destroy(v->window);
	xdg_surface_destroy(v->shell);
	wl_surface_destroy(v->surface);
	for (size_t i = 0; i < 2; ++i)
		if (v->frames[i]) released(v->frames[i], v->frames[i]->buffer);
	*v = (struct view){ .width = v->width, .height = v->height };
}

static void redraw(struct view *v)
{
	char path[] = "/tmp/ludus-XXXXXX";
	struct frame *frame;
	struct wl_shm_pool *pool;
	int fd;
	int width = v->width, height = v->height;
	if (!v->configured || !v->dirty) return;
	struct frame **slot = v->frames[0] ? &v->frames[1] : &v->frames[0];
	if (*slot) return;
	if ((int64_t)width * height > INT32_MAX / 4)
		die("window too large for a Wayland shared memory buffer");
	frame = calloc(1, sizeof(*frame));
	if (!frame) die("out of memory");
	frame->slot = slot;
	*slot = frame;
	frame->bytes = (size_t)width * height * 4;
	fd = mkstemp(path);
	if (fd < 0) die(strerror(errno));
	unlink(path);
	if (ftruncate(fd, frame->bytes) < 0) die(strerror(errno));
	frame->pixels = mmap(NULL, frame->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (frame->pixels == MAP_FAILED) die(strerror(errno));
	pool = wl_shm_create_pool(shm, fd, frame->bytes);
	frame->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4,
	                                          WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(frame->buffer, &buffer_events, frame);
	struct canvas c = {frame->pixels, width, height};
	paint(c);
	wl_surface_attach(v->surface, frame->buffer, 0, 0);
	wl_surface_damage(v->surface, 0, 0, width, height);
	wl_surface_commit(v->surface);
	v->dirty = 0;
}

static size_t previous_char(const char *s, size_t pos)
{
	if (!pos) return 0;
	while (--pos && ((unsigned char)s[pos] & 0xc0) == 0x80) {}
	return pos;
}

static size_t next_char(const char *s, size_t pos)
{
	if (!s[pos]) return pos;
	do { ++pos; } while (((unsigned char)s[pos] & 0xc0) == 0x80);
	return pos;
}

static int insert_text(char *value, size_t *cursor_pos, const char *str)
{
	size_t pos = *cursor_pos, len = strlen(value), n = strlen(str);
	int chars;
	int size;
	if (n >= PATH_MAX - len || !FcUtf8Len((const FcChar8 *)str, (int)n, &chars, &size)) return 0;
	memmove(value + pos + n, value + pos, len - pos + 1);
	memcpy(value + pos, str, n);
	*cursor_pos += n;
	return 1;
}

static void edit_line(char *value, size_t *cursor_pos, xkb_keysym_t sym, uint32_t code)
{
	char typed[64];
	size_t pos = *cursor_pos, len = strlen(value);
	switch (sym) {
	case XKB_KEY_Left: *cursor_pos = previous_char(value, pos); return;
	case XKB_KEY_Right: *cursor_pos = next_char(value, pos); return;
	case XKB_KEY_Home: *cursor_pos = 0; return;
	case XKB_KEY_End: *cursor_pos = len; return;
	case XKB_KEY_BackSpace:
		*cursor_pos = previous_char(value, pos);
		memmove(value + *cursor_pos, value + pos, len - pos + 1);
		return;
	case XKB_KEY_Delete: {
		size_t next = next_char(value, pos);
		memmove(value + pos, value + next, len - next + 1);
		return;
	}
	}
	int n = xkb_state_key_get_utf8(keys, code, typed, sizeof typed);
	if (n > 0 && n < (int)sizeof typed && (unsigned char)typed[0] >= 32 && typed[0] != 127)
		insert_text(value, cursor_pos, typed);
}

/* Search moves the grid selection; n/N continue from it and wrap around. */
static int find_title(int start, int direction)
{
	int i = start;
	for (int n = 0; n < count; ++n) {
		if (i < 0) i = count - 1;
		if (i >= count) i = 0;
		if (FcStrStrIgnoreCase((const FcChar8 *)games[i].name, (const FcChar8 *)query)) {
			selected = i;
			return 1;
		}
		i += direction;
	}
	return 0;
}

static void ignore_terminal_signal(int sig)
{
	(void)sig;
}

static void restore_editor_signals(size_t count)
{
	while (count) {
		--count;
		sigaction(editor_signals[count], &editor_handlers[count], NULL);
	}
}

static int spawn_tty_editor(int tty, char **args)
{
	posix_spawn_file_actions_t io;
	int error = posix_spawn_file_actions_init(&io);
	if (error) return error;
	for (int fd = STDIN_FILENO; !error && fd <= STDERR_FILENO; ++fd)
		error = posix_spawn_file_actions_adddup2(&io, tty, fd);

	/* The shell and editor get default dispositions for these caught signals. */
	struct sigaction action = { .sa_handler = ignore_terminal_signal };
	sigemptyset(&action.sa_mask);
	size_t count = 0;
	while (!error && count < sizeof editor_signals / sizeof *editor_signals) {
		if (sigaction(editor_signals[count], &action, &editor_handlers[count])) error = errno;
		else ++count;
	}
	if (!error) error = posix_spawn(&editor_pid, args[0], &io, NULL, args, environ);
	posix_spawn_file_actions_destroy(&io);
	if (error) restore_editor_signals(count);
	return error;
}

static int spawn_terminal(char *command, const char *path)
{
	const char *terminal = getenv("TERMINAL");
	if (terminal && *terminal) {
		char wrapper[PATH_MAX];
		if (snprintf(wrapper, sizeof wrapper, "exec %s \"$@\"", terminal) >= (int)sizeof wrapper)
			return ENAMETOOLONG;
		char *args[] = {"/bin/sh", "-c", wrapper, "ludus-terminal",
		                "/bin/sh", "-c", command, "ludus-editor", (char *)path, NULL};
		return posix_spawn(&editor_pid, args[0], NULL, NULL, args, environ);
	}
	char *args[] = {"xdg-terminal-exec", "/bin/sh", "-c", command, "ludus-editor", (char *)path, NULL};
	return posix_spawnp(&editor_pid, args[0], NULL, NULL, args, environ);
}

static void start_editor(enum editor_action action, const char *target)
{
	if (editor_pid) return;
	main_view.dirty = 1;
	const char *editor = getenv("EDITOR");
	const char *path = target;
	char *message = status;
	size_t capacity = sizeof status;
	if (action == AddConfig) { message = prompt_error; capacity = sizeof prompt_error; }
	if (prompt_mode == RepairPrompt) { message = repair_message; capacity = sizeof repair_message; }
	char command[PATH_MAX];
	if (!editor || !*editor) {
		strcpy(message, "Set EDITOR to edit game configs");
		return;
	}
	/* The editor command can contain options. The filename is a separate argument. */
	if (snprintf(command, sizeof command, "exec %s \"$1\"", editor) >= (int)sizeof command) {
		strcpy(message, "EDITOR command is too long");
		return;
	}
	if (action == AddConfig) {
		if (library_draft(target, editor_config, &draft_revision)) {
			if (errno == EEXIST) strcpy(message, "A config with that name exists");
			else if (errno == EINVAL) strcpy(message, "Enter a valid filename");
			else snprintf(message, capacity, "Cannot create config: %s", strerror(errno));
			return;
		}
		path = editor_config;
	} else if (action == EditConfig) {
		const char *missing;
		if (library_inspect(path, &missing, &previous_hash)) {
			snprintf(message, capacity, "Cannot read config: %s", strerror(errno));
			return;
		}
		if (target != editor_config) snprintf(editor_config, sizeof editor_config, "%s", target);
	}
	stop_repeat();
	clipboard_cancel();
	focus = NULL;
	destroy_view(&main_view);
	wl_display_flush(display);

	char *args[] = {"/bin/sh", "-c", command, "ludus-editor", (char *)path, NULL};
	int tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (tty >= 0 && (tcgetpgrp(tty) != getpgrp() || tcgetattr(tty, &editor_termios))) {
		close(tty);
		tty = -1;
	}
	int error = tty >= 0 ? spawn_tty_editor(tty, args) : spawn_terminal(command, path);
	if (error) {
		if (tty >= 0) close(tty);
		if (action == AddConfig) library_discard_draft(editor_config, &draft_revision);
		editor_pid = 0;
		if (tty < 0 && error == ENOENT)
			strcpy(message, "Set TERMINAL or install xdg-terminal-exec");
		else
			snprintf(message, capacity, "Cannot start %s: %s",
			         tty >= 0 ? "editor" : "terminal", strerror(error));
		show_view();
		return;
	}
	editor_action = action;
	editor_tty = tty;
	prompt_mode = NoPrompt;
	status[0] = 0;
}

static void rename_selected(void)
{
	char path[PATH_MAX];
	if (library_rename(&games[selected], game_name, path)) {
		if (errno == EEXIST) strcpy(prompt_error, "A config with that name exists");
		else if (errno == EINVAL) strcpy(prompt_error, "Enter a valid filename");
		else if (errno == ESTALE || errno == ENOENT) strcpy(prompt_error, "Config changed. Esc, then r");
		else snprintf(prompt_error, sizeof prompt_error, "Cannot rename config: %s", strerror(errno));
		return;
	}
	prompt_mode = NoPrompt;
	snprintf(status, sizeof status, "Renamed to %s", game_name);
	selected = library_load(selected, path);
}

static void remove_selected(void)
{
	prompt_mode = NoPrompt;
	if (library_remove(&games[selected])) {
		if (errno == ESTALE || errno == ENOENT) strcpy(status, "Config changed. Press r to refresh");
		else snprintf(status, sizeof status, "Cannot remove config: %s", strerror(errno));
		return;
	}
	snprintf(status, sizeof status, "Removed %s", games[selected].name);
	selected = library_load(selected, NULL);
}

static void prompt_key(xkb_keysym_t sym, uint32_t code)
{
	main_view.dirty = 1;
	int cancel = sym == XKB_KEY_Escape;
	int search = prompt_mode == SearchPrompt;
	if (cancel || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		clipboard_cancel();
		if (!search && !cancel) {
			if (prompt_mode == RemovePrompt) remove_selected();
			else if (prompt_mode == RenamePrompt) rename_selected();
			else if (*game_name) start_editor(AddConfig, game_name);
			else strcpy(prompt_error, "Enter a game name");
			return;
		}
		if (search && (!found || cancel)) selected = search_origin;
		if (search && cancel) query[0] = 0;
		prompt_mode = NoPrompt;
		return;
	}
	edit_line(search ? query : game_name, &input_cursor, sym, code);
	if (search) found = find_title(search_origin, 1);
	else prompt_error[0] = 0;
}

static void editor_finished(int result)
{
	editor_pid = 0;
	if (editor_tty >= 0) {
		if (tcgetpgrp(editor_tty) == getpgrp())
			tcsetattr(editor_tty, TCSANOW, &editor_termios);
		close(editor_tty);
		editor_tty = -1;
		restore_editor_signals(sizeof editor_signals / sizeof *editor_signals);
	}
	int discarded = editor_action == AddConfig && library_discard_draft(editor_config, &draft_revision);
	selected = library_load(selected, editor_action == ViewLog ? NULL : editor_config);
	if (WIFSIGNALED(result))
		snprintf(status, sizeof status, "Editor stopped (signal %d)", WTERMSIG(result));
	else if (WIFEXITED(result) && WEXITSTATUS(result))
		snprintf(status, sizeof status, "Editor exited (%d)", WEXITSTATUS(result));
	if (editor_action != ViewLog) {
		const char *name = strrchr(editor_config, '/') + 1;
		const char *missing;
		uint64_t hash;
		int success = WIFEXITED(result) && !WEXITSTATUS(result);
		if (discarded) {
			if (success) strcpy(status, "No changes");
		} else if (library_inspect(editor_config, &missing, &hash)) {
			if (success) snprintf(status, sizeof status, "Cannot read %s: %s", name, strerror(errno));
		} else if (missing) {
			char key[64] = {0};
			xkb_keysym_get_name(KEY_EDIT, key, sizeof key);
			snprintf(repair_message, sizeof repair_message, "Missing %s=. %s to reopen, Esc to dismiss. %s",
			         missing, key, name);
			prompt_mode = RepairPrompt;
		} else if (success) {
			if (editor_action == AddConfig) snprintf(status, sizeof status, "Added %s", name);
			else if (hash != previous_hash) snprintf(status, sizeof status, "Updated %s", name);
			else strcpy(status, "No changes");
		}
	}
	show_view();
}

static void reap(void)
{
	int result;
	pid_t pid;
	while ((pid = waitpid(-1, &result, WNOHANG)) > 0) {
		if (pid == editor_pid) { editor_finished(result); continue; }
		--active;
		for (int i = 0; i < count; ++i) {
			if (games[i].pid != pid) continue;
			games[i].pid = 0;
			if (WIFEXITED(result) && !WEXITSTATUS(result)) {
				snprintf(status, sizeof status, "Stopped %s", games[i].name);
			} else {
				char message[sizeof status - sizeof "Launch failed: "];
				game_error(&games[i], message, sizeof message);
				if (*message) snprintf(status, sizeof status, "Launch failed: %s", message);
				else if (WIFSIGNALED(result))
					snprintf(status, sizeof status, "%s stopped (signal %d)", games[i].name, WTERMSIG(result));
				else snprintf(status, sizeof status, "%s failed (exit %d)", games[i].name, WEXITSTATUS(result));
			}
			main_view.dirty = 1;
			break;
		}
	}
}

static void press(xkb_keysym_t sym)
{
	int col = columns(), old = selected;
	if (sym == KEY_FIRST && count) {
		if (first_key_pending) { selected = 0; main_view.dirty = 1; }
		first_key_pending = !first_key_pending;
		return;
	}
	first_key_pending = 0;
	if (sym == KEY_HELP) { help_page = 1; main_view.dirty = 1; return; }
	if (sym == KEY_QUIT) running = 0;
	if (sym == KEY_REFRESH) {
		selected = library_load(selected, NULL);
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_ADD || sym == XKB_KEY_KP_Add) {
		game_name[0] = prompt_error[0] = 0;
		input_cursor = 0;
		prompt_mode = NamePrompt;
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_SEARCH) {
		search_origin = selected;
		query[0] = 0;
		input_cursor = 0;
		found = count > 0;
		prompt_mode = SearchPrompt;
		main_view.dirty = 1;
		return;
	}
	if (!count) return;
	if (sym == KEY_RENAME) {
		snprintf(game_name, sizeof game_name, "%s", games[selected].name);
		input_cursor = strlen(game_name);
		prompt_error[0] = 0;
		prompt_mode = RenamePrompt;
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_REMOVE) {
		prompt_mode = RemovePrompt;
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_LOG) {
		if (games[selected].log) start_editor(ViewLog, games[selected].log);
		else { strcpy(status, "No launch log for this game"); main_view.dirty = 1; }
		return;
	}
	if (*query && (sym == KEY_NEXT_MATCH || sym == KEY_PREV_MATCH)) {
		int direction = sym == KEY_NEXT_MATCH ? 1 : -1;
		find_title(selected + direction, direction);
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_EDIT) { start_editor(EditConfig, games[selected].path); return; }
	if (sym == KEY_LAST) selected = count - 1;
	if (sym == KEY_LEFT && selected % col > 0) --selected;
	if (sym == KEY_RIGHT && selected % col < col - 1 && selected + 1 < count) ++selected;
	if (sym == KEY_UP && selected >= col) selected -= col;
	if (sym == KEY_DOWN && selected / col < (count - 1) / col) {
		selected += col;
		if (selected >= count) selected = count - 1;
	}
	if (sym == KEY_LAUNCH) {
		struct game *g = &games[selected];
		int error = game_launch(g);
		if (error == EALREADY || !error)
			snprintf(status, sizeof status, "Playing %s", g->name);
		else {
			snprintf(status, sizeof status, "Cannot start oyna: %s", strerror(error));
			fprintf(stderr, "ludus: %s: %s\n", g->name, status);
		}
		if (!error) ++active;
		main_view.dirty = 1;
	}
	if (selected != old) main_view.dirty = 1;
}

static void keymap(void *data, struct wl_keyboard *kb, uint32_t format, int fd, uint32_t size)
{
	char *text;
	struct xkb_keymap *map;
	(void)data; (void)kb;
	stop_repeat();
	xkb_state_unref(keys);
	keys = NULL;
	if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size) {
		text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (text == MAP_FAILED) die(strerror(errno));
		map = xkb_keymap_new_from_buffer(xkb, text, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
		                                 XKB_KEYMAP_COMPILE_NO_FLAGS);
		munmap(text, size);
		if (!map) die("invalid compositor keymap");
		keys = xkb_state_new(map);
		xkb_keymap_unref(map);
		if (!keys) die("cannot create keyboard state");
	}
	close(fd);
}
static void modifiers(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t depressed,
                      uint32_t latched, uint32_t locked, uint32_t group)
{
	(void)data; (void)kb; (void)serial;
	if (keys && xkb_state_update_mask(keys, depressed, latched, locked, 0, 0, group)) stop_repeat();
}
static int paste_shortcut(xkb_keysym_t sym)
{
	return (sym == XKB_KEY_v && xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) ||
	       (sym == XKB_KEY_Insert && xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0);
}

static int handle_press(uint32_t code)
{
	xkb_keysym_t sym = xkb_state_key_get_one_sym(keys, code);
	if (!main_view.surface || focus != main_view.surface) return 0;
	if (help_page) {
		if (sym == KEY_HELP || sym == XKB_KEY_Escape) help_page = 0;
		else if (sym == XKB_KEY_space) help_page = help_page % help_pages() + 1;
		else return 0;
		main_view.dirty = 1;
		return 0;
	}
	if (prompt_mode == RepairPrompt) {
		if (sym == KEY_EDIT) start_editor(EditConfig, editor_config);
		else if (sym == XKB_KEY_Escape) { prompt_mode = NoPrompt; main_view.dirty = 1; }
		return 0;
	}
	if (prompt_mode == RemovePrompt) {
		if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter)
			prompt_key(sym, code);
		return 0;
	}
	if (prompt_mode != NoPrompt) {
		int submit = sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter;
		if (submit && clipboard_fd() >= 0) {
			submit_after_paste = 1;
			return 0;
		}
		if (submit_after_paste && sym != XKB_KEY_Escape) return 0;
		if (sym == XKB_KEY_Escape) submit_after_paste = 0;
		if (paste_shortcut(sym)) {
			clipboard_receive();
			return 0;
		}
		prompt_key(sym, code);
		return !submit && sym != XKB_KEY_Escape && sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab;
	}
	press(sym);
	return sym == KEY_LEFT || sym == KEY_RIGHT || sym == KEY_UP || sym == KEY_DOWN;
}

static void key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                uint32_t code, uint32_t state)
{
	(void)data; (void)kb; (void)serial; (void)time;
	code += 8;
	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		if (code == repeat_code) stop_repeat();
		return;
	}
	stop_repeat();
	if (!keys || !handle_press(code) || repeat_rate <= 0 ||
	    !xkb_keymap_key_repeats(xkb_state_get_keymap(keys), code)) return;
	struct itimerspec timer = {0};
	long period = 1000000000L / repeat_rate;
	if (!period) period = 1;
	timer.it_interval.tv_sec = period / 1000000000L;
	timer.it_interval.tv_nsec = period % 1000000000L;
	timer.it_value.tv_sec = repeat_delay / 1000;
	timer.it_value.tv_nsec = (repeat_delay % 1000) * 1000000L;
	if (!repeat_delay) timer.it_value.tv_nsec = 1;
	if (timerfd_settime(repeat_fd, 0, &timer, NULL)) die(strerror(errno));
	repeat_code = code;
}

static void repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)data; (void)kb;
	stop_repeat();
	repeat_rate = rate;
	repeat_delay = delay < 0 ? 0 : delay;
}
static void enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w, struct wl_array *a)
{ (void)d; (void)k; (void)s; stop_repeat(); focus = w; (void)a; }
static void leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w)
{ (void)d; (void)k; (void)s; stop_repeat(); if (focus == w) { focus = NULL; first_key_pending = 0; } }
static const struct wl_keyboard_listener keyboard_events = {
	.keymap = keymap, .enter = enter, .leave = leave, .key = key, .modifiers = modifiers, .repeat_info = repeat_info
};

static void capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_events, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
		stop_repeat();
		if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(keyboard);
		else wl_keyboard_destroy(keyboard);
		keyboard = NULL;
	}
}
static void seat_named(void *data, struct wl_seat *s, const char *name)
{ (void)data; (void)s; (void)name; }
static const struct wl_seat_listener seat_events = { .capabilities = capabilities, .name = seat_named };

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial)
{ (void)data; xdg_wm_base_pong(base, serial); }
static const struct xdg_wm_base_listener wm_events = { .ping = ping };

static void global(void *data, struct wl_registry *r, uint32_t id, const char *name, uint32_t version)
{
	(void)data; (void)version;
	if (!strcmp(name, "wl_compositor"))
		compositor = wl_registry_bind(r, id, &wl_compositor_interface, 1);
	else if (!strcmp(name, "wl_shm"))
		shm = wl_registry_bind(r, id, &wl_shm_interface, 1);
	else if (!strcmp(name, "xdg_wm_base")) {
		wm = wl_registry_bind(r, id, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(wm, &wm_events, NULL);
	} else if (!strcmp(name, "wl_data_device_manager")) {
		clipboard_bind(r, id);
	} else if (!seat && !strcmp(name, "wl_seat")) {
		seat = wl_registry_bind(r, id, &wl_seat_interface, version < 4 ? version : 4);
		repeat_rate = 25;
		repeat_delay = 600;
		seat_name = id;
		clipboard_seat(seat);
		wl_seat_add_listener(seat, &seat_events, NULL);
	}
}
static void removed(void *data, struct wl_registry *r, uint32_t id)
{
	(void)data; (void)r;
	if (seat && id == seat_name) {
		capabilities(NULL, seat, 0);
		clipboard_seat(NULL);
		submit_after_paste = 0;
		wl_seat_destroy(seat);
		seat = NULL;
	}
}
static const struct wl_registry_listener registry_events = { .global = global, .global_remove = removed };

static void configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	struct view *v = data;
	xdg_surface_ack_configure(s, serial);
	v->configured = v->dirty = 1;
}
static const struct xdg_surface_listener surface_events = { .configure = configure };

static void resize(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
	struct view *v = data;
	(void)t; (void)states;
	if (w > 0) v->width = w;
	if (h > 0) v->height = h;
}
static void closed(void *data, struct xdg_toplevel *t)
{ (void)t; (void)data; running = 0; }
static const struct xdg_toplevel_listener window_events = { .configure = resize, .close = closed };

static void show_view(void)
{
	struct view *v = &main_view;
	v->surface = wl_compositor_create_surface(compositor);
	v->shell = xdg_wm_base_get_xdg_surface(wm, v->surface);
	xdg_surface_add_listener(v->shell, &surface_events, v);
	v->window = xdg_surface_get_toplevel(v->shell);
	xdg_toplevel_add_listener(v->window, &window_events, v);
	xdg_toplevel_set_title(v->window, "ludus");
	xdg_toplevel_set_app_id(v->window, "ludus");
	xdg_toplevel_set_min_size(v->window, COVER_WIDTH + 2 * SPACING,
	                          Header + Footer + TileHeight + 2 * SPACING);
	wl_surface_commit(v->surface);
}

static void finish_paste(void)
{
	const char *s = clipboard_read();
	if (clipboard_fd() >= 0) return;
	int submit = submit_after_paste;
	submit_after_paste = 0;
	if (prompt_mode != NoPrompt) {
		int search = prompt_mode == SearchPrompt;
		if (s && insert_text(search ? query : game_name, &input_cursor, s)) {
			if (search) found = find_title(search_origin, 1);
			else prompt_error[0] = 0;
			if (submit) prompt_key(XKB_KEY_Return, 0);
		}
		main_view.dirty = 1;
	}
}

static void event_loop(void)
{
	struct pollfd fds[] = {
		{.fd = wl_display_get_fd(display)}, {.events = POLLIN}, {.fd = repeat_fd, .events = POLLIN}
	};
	while (running) {
		if (wl_display_dispatch_pending(display) < 0) die("Wayland connection lost");
		if (!running) break;
		if (active || editor_pid) reap();
		redraw(&main_view);
		fds[0].events = POLLIN;
		if (wl_display_flush(display) < 0) {
			if (errno != EAGAIN) die("Wayland connection lost");
			fds[0].events |= POLLOUT;
		}
		fds[1].fd = clipboard_fd();
		if (poll(fds, 3, (active || editor_pid) ? 150 : -1) < 0) {
			if (errno == EINTR) continue;
			die(strerror(errno));
		}
		if (fds[1].revents) finish_paste();
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) die("Wayland connection lost");
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0) die("Wayland connection lost");
		if (fds[2].revents & POLLIN) {
			uint64_t ticks;
			if (read(repeat_fd, &ticks, sizeof ticks) == sizeof ticks &&
			    repeat_code && !handle_press(repeat_code)) stop_repeat();
		}
	}
}

int main(void)
{
	struct wl_registry *registry;
	struct sigaction action = { .sa_handler = SIG_DFL };
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCHLD, &action, NULL)) die(strerror(errno));
	repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (repeat_fd < 0) die(strerror(errno));
	font_init(FONT_PATTERN);
	selected = library_load(0, NULL);
	display = wl_display_connect(NULL);
	if (!display) die("cannot connect to Wayland display");
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb) die("cannot initialize xkbcommon");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_events, NULL);
	if (wl_display_roundtrip(display) < 0) die("cannot read Wayland registry");
	if (!compositor || !shm || !wm) die("compositor lacks wl_compositor, wl_shm or xdg-shell");
	show_view();
	event_loop();
	if (main_view.surface) destroy_view(&main_view);
	clipboard_finish();
	if (seat) {
		capabilities(NULL, seat, 0);
		wl_seat_destroy(seat);
	}
	xdg_wm_base_destroy(wm);
	wl_shm_destroy(shm);
	wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_display_disconnect(display);
	close(repeat_fd);
	xkb_state_unref(keys);
	xkb_context_unref(xkb);
	library_finish();
	font_finish();
	return 0;
}

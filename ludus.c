#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

enum field { Name, Art, FieldCount };
enum {
	CoverHeight = COVER_WIDTH * 3 / 2,
	TitleLines = 3,
	TileHeight = CoverHeight + 6 + TitleLines * (FONT_SIZE + 3),
	Header = HEADER_FONT_SIZE + 20,
	Footer = STATUS_FONT_SIZE + 6,
	Row = FONT_SIZE * 2 + 12,
	DialogHeight = FieldCount * Row + 8
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
static struct view main_view = { .width = 900, .height = 640 }, dialog;
static struct wl_surface *focus;
static int selected, first_row, running = 1, active;
static char fields[FieldCount][PATH_MAX], form_error[256], status[PATH_MAX + 128];
static int searching, search_origin, found, paste_target;
static char query[PATH_MAX];
static size_t query_cursor;
static int submit_after_paste;
static enum field field;
static size_t cursor[FieldCount];
static struct game *editing;
static const char *labels[] = {"Name", "Artwork"};
static void open_dialog(struct game *g);
static void close_dialog(void);
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

static void paint(struct canvas c)
{
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
	text(c, SPACING, HEADER_FONT_SIZE + 6, 0, width, HEADER_FONT_SIZE, 0xffffff, "ludus");
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
			fill(c, left - 2, top - 2, COVER_WIDTH + 4, CoverHeight + 4, BORDER_COLOR);
		if (!games[i].tried) load_cover(&games[i]);
		wrapped_title(c, left, top + CoverHeight + 6 + FONT_SIZE, COVER_WIDTH, FONT_SIZE,
		              TitleLines, 0xffffff, games[i].name);
		if (!games[i].thumbnail) {
			fill(c, left, top, COVER_WIDTH, CoverHeight, 0x38393b);
			continue;
		}
		for (y = 0; y < CoverHeight && top + y < bar; ++y)
			memcpy(c.pixels + (size_t)(top + y) * width + left,
			       games[i].thumbnail + y * COVER_WIDTH, visible * sizeof(*c.pixels));
	}
	for (i = cached_begin; i < cached_end; ++i) {
		if (i >= begin && i < end) continue;
		free(games[i].thumbnail);
		games[i].thumbnail = NULL;
		games[i].tried = 0;
	}
	cached_begin = begin;
	cached_end = end;
	fill(c, 0, bar, width, Footer, BAR_BACKGROUND);
	if (searching) {
		const char *match = found ? games[selected].name : "No match";
		char prompt[sizeof query + 2];
		snprintf(prompt, sizeof prompt, "/%s", query);
		int edge = width / 2;
		char saved = prompt[1 + query_cursor];
		prompt[1 + query_cursor] = 0;
		int caret = text_width(STATUS_FONT_SIZE, prompt);
		prompt[1 + query_cursor] = saved;
		int start = caret + 8 < edge ? 6 : edge - caret - 2;
		text(c, start, bar + STATUS_FONT_SIZE + 1, 6, edge, STATUS_FONT_SIZE, 0xffffff, prompt);
		fill(c, start + caret, bar + 3, 1, STATUS_FONT_SIZE, BORDER_COLOR);
		text(c, edge + 6, bar + STATUS_FONT_SIZE + 1, edge + 6, right, STATUS_FONT_SIZE, 0xffffff, match);
		return;
	}
	text(c, 6, bar + STATUS_FONT_SIZE + 1, 0, split - 4, STATUS_FONT_SIZE, 0xffffff,
	     count ? games[selected].name : "Put oyna configs in ~/.ludus/");
	if (*status)
		text(c, status_x, bar + STATUS_FONT_SIZE + 1, split, right, STATUS_FONT_SIZE, 0xffffff, status);

}

static void paint_dialog(struct canvas c)
{
	int left = 14, right = c.width - 14;
	int step = (c.height - 8) / FieldCount;
	fill(c, 0, 0, c.width, c.height, BAR_BACKGROUND);
	for (int i = 0; i < FieldCount; ++i) {
		int y = 4 + i * step, start = left + 5;
		int input_y = y + FONT_SIZE + 2;
		int available = right - start - 5, offset = 0;
		int focused = i == (int)field;
		fill(c, left, input_y, right - left, step - FONT_SIZE - 4, focused ? BORDER_COLOR : 0x555555);
		fill(c, left + 1, input_y + 1, right - left - 2, step - FONT_SIZE - 6, 0);
		text(c, left, y + FONT_SIZE - 1, left, right, FONT_SIZE, 0xffffff, labels[i]);
		if (focused) {
			char saved = fields[i][cursor[i]];
			fields[i][cursor[i]] = 0;
			offset = text_width(FONT_SIZE, fields[i]);
			fields[i][cursor[i]] = saved;
			if (offset > available) start -= offset - available;
			int label_end = left + text_width(FONT_SIZE, labels[i]) + 10;
			text(c, label_end, y + FONT_SIZE - 1, label_end, right, FONT_SIZE, 0xffffff, form_error);
		}
		text(c, start, input_y + FONT_SIZE + 1, left + 4, right - 2, FONT_SIZE, 0xffffff, fields[i]);
		if (focused) fill(c, start + offset, input_y + 3, 1, FONT_SIZE, BORDER_COLOR);
	}
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
	*v = (struct view){0};
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
	if (v == &dialog) paint_dialog(c);
	else paint(c);
	wl_surface_attach(v->surface, frame->buffer, 0, 0);
	wl_surface_damage(v->surface, 0, 0, width, height);
	wl_surface_commit(v->surface);
	v->dirty = 0;
}

static void save_form(void)
{
	if (!*fields[Name]) {
		strcpy(form_error, "Enter a name.");
		field = Name;
		return;
	}
	if (!library_save(editing, fields[Name], fields[Art])) {
		close_dialog();
	} else if (errno == EEXIST) {
		field = Name;
		strcpy(form_error, "A config with this name already exists.");
	} else if (errno == ESTALE) {
		strcpy(form_error, "File changed. Esc, then reopen to reload.");
	} else if (errno == EINVAL) {
		field = Name;
		strcpy(form_error, "Use a visible filename without / or .meta.");
	} else snprintf(form_error, sizeof form_error, "Save failed: %s", strerror(errno));
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

static void form_key(xkb_keysym_t sym, uint32_t code)
{
	dialog.dirty = 1;
	form_error[0] = 0;
	switch (sym) {
	case XKB_KEY_Escape: close_dialog(); return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: save_form(); return;
	case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab:
		field = field == Name ? Art : Name;
		return;
	}
	edit_line(fields[field], &cursor[field], sym, code);
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

static void search_key(xkb_keysym_t sym, uint32_t code)
{
	main_view.dirty = 1;
	if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		clipboard_cancel();
		if (!found || sym == XKB_KEY_Escape) selected = search_origin;
		searching = 0;
		if (sym == XKB_KEY_Escape) query[0] = 0;
		return;
	}
	edit_line(query, &query_cursor, sym, code);
	found = find_title(search_origin, 1);
}

static void reap(void)
{
	int result;
	pid_t pid;
	while ((pid = waitpid(-1, &result, WNOHANG)) > 0) {
		for (int i = 0; i < count; ++i) {
			if (games[i].pid != pid) continue;
			games[i].pid = 0;
			--active;
			snprintf(status, sizeof status, "Stopped %s", games[i].name);
			if (WIFEXITED(result) && WEXITSTATUS(result))
				snprintf(status, sizeof status, "Stopped %s (exit %d)", games[i].name, WEXITSTATUS(result));
			main_view.dirty = 1;
			break;
		}
	}
}

static void press(xkb_keysym_t sym)
{
	int col = columns(), old = selected;
	if (sym == KEY_QUIT) running = 0;
	if (sym == KEY_SEARCH) {
		search_origin = selected;
		query[0] = 0;
		query_cursor = 0;
		found = count > 0;
		searching = main_view.dirty = 1;
		return;
	}
	if (!count) return;
	if (*query && (sym == KEY_NEXT_MATCH || sym == KEY_PREV_MATCH)) {
		int direction = sym == KEY_NEXT_MATCH ? 1 : -1;
		find_title(selected + direction, direction);
		main_view.dirty = 1;
		return;
	}
	if (sym == KEY_EDIT) { open_dialog(&games[selected]); return; }
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
	int in_form = dialog.surface && focus == dialog.surface;
	int in_search = searching && focus == main_view.surface;
	if (in_form || in_search) {
		int submit = sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter;
		if (submit && clipboard_fd() >= 0) {
			submit_after_paste = 1;
			return 0;
		}
		if (submit_after_paste && sym != XKB_KEY_Escape) return 0;
		if (sym == XKB_KEY_Escape) submit_after_paste = 0;
		if (paste_shortcut(sym)) {
			paste_target = in_form ? (int)field : FieldCount;
			clipboard_receive();
			return 0;
		}
		if (in_form) form_key(sym, code);
		else search_key(sym, code);
		return !submit && sym != XKB_KEY_Escape && sym != XKB_KEY_Tab && sym != XKB_KEY_ISO_Left_Tab;
	}
	if (dialog.surface || focus != main_view.surface) return 0;
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
{ (void)d; (void)k; (void)s; stop_repeat(); if (focus == w) focus = NULL; }
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
{ (void)t; if (data == &dialog) close_dialog(); else running = 0; }
static const struct xdg_toplevel_listener window_events = { .configure = resize, .close = closed };

static void create_view(struct view *v, const char *title)
{
	v->surface = wl_compositor_create_surface(compositor);
	v->shell = xdg_wm_base_get_xdg_surface(wm, v->surface);
	xdg_surface_add_listener(v->shell, &surface_events, v);
	v->window = xdg_surface_get_toplevel(v->shell);
	xdg_toplevel_add_listener(v->window, &window_events, v);
	xdg_toplevel_set_title(v->window, title);
	xdg_toplevel_set_app_id(v->window, "ludus");
}

static void open_dialog(struct game *g)
{
	if (library_refresh(g)) {
		snprintf(status, sizeof status, "Cannot read entry: %s", strerror(errno));
		main_view.dirty = 1;
		return;
	}
	const char *values[FieldCount] = {g->name, g->art};
	for (int i = 0; i < FieldCount; ++i) {
		if (strlen(values[i]) >= sizeof fields[i] || strchr(values[i], '\n')) {
			strcpy(status, "Entry exceeds the single-line field limits");
			main_view.dirty = 1;
			return;
		}
		strcpy(fields[i], values[i]);
		cursor[i] = strlen(fields[i]);
	}
	editing = g;
	field = Name;
	form_error[0] = 0;
	dialog.width = 520;
	dialog.height = DialogHeight;
	create_view(&dialog, "Edit game");
	xdg_toplevel_set_parent(dialog.window, main_view.window);
	xdg_toplevel_set_min_size(dialog.window, 420, DialogHeight);
	xdg_toplevel_set_max_size(dialog.window, 520, DialogHeight);
	wl_surface_commit(dialog.surface);
}

static void close_dialog(void)
{
	clipboard_cancel();
	if (focus == dialog.surface) focus = NULL;
	stop_repeat();
	submit_after_paste = 0;
	destroy_view(&dialog);
	main_view.dirty = 1;
}

static void finish_paste(void)
{
	const char *s = clipboard_read();
	if (clipboard_fd() >= 0) return;
	int submit = submit_after_paste;
	submit_after_paste = 0;
	if (searching && paste_target == FieldCount) {
		if (s && insert_text(query, &query_cursor, s)) {
			found = find_title(search_origin, 1);
			if (submit) search_key(XKB_KEY_Return, 0);
		}
		main_view.dirty = 1;
	} else if (dialog.surface && paste_target < FieldCount) {
		if (!s || !insert_text(fields[paste_target], &cursor[paste_target], s))
			strcpy(form_error, "Cannot paste this text.");
		else if (submit) save_form();
		dialog.dirty = 1;
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
		if (active) reap();
		redraw(&main_view);
		redraw(&dialog);
		fds[0].events = POLLIN;
		if (wl_display_flush(display) < 0) {
			if (errno != EAGAIN) die("Wayland connection lost");
			fds[0].events |= POLLOUT;
		}
		fds[1].fd = clipboard_fd();
		if (poll(fds, 3, active ? 150 : -1) < 0) {
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
	library_load();
	display = wl_display_connect(NULL);
	if (!display) die("cannot connect to Wayland display");
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb) die("cannot initialize xkbcommon");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_events, NULL);
	if (wl_display_roundtrip(display) < 0) die("cannot read Wayland registry");
	if (!compositor || !shm || !wm) die("compositor lacks wl_compositor, wl_shm or xdg-shell");
	create_view(&main_view, "ludus");
	xdg_toplevel_set_min_size(main_view.window, COVER_WIDTH + 2 * SPACING,
	                          Header + Footer + TileHeight + 2 * SPACING);
	wl_surface_commit(main_view.surface);
	event_loop();
	if (dialog.surface) close_dialog();
	destroy_view(&main_view);
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

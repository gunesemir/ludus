#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <poll.h>
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
	int width, height, configured, dirty, busy;
};
struct frame {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t bytes;
	struct view *owner;
};
static struct view main_view = { .width = 900, .height = 640 }, dialog;
static struct wl_surface *focus;
static int selected, first_row, running = 1, active;
static char fields[FieldCount][PATH_MAX], form_error[256], status[PATH_MAX + 128];
static int searching, search_origin, found, paste_target;
static char query[PATH_MAX];
static size_t query_cursor;
static enum field field;
static size_t cursor[FieldCount];
static int editing;
static const char *labels[] = {"Name", "Executable", "Artwork", "Runner", "WINEPREFIX", "PROTONPATH", "Launch command"};
static void open_dialog(int index);
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
		              TitleLines, 0xffffff, games[i].value[Name]);
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
		const char *match = found ? games[selected].value[Name] : "No match";
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
	     count ? games[selected].value[Name] : "+ Add game");
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
	--frame->owner->busy;
	free(frame);
}
static const struct wl_buffer_listener buffer_events = { .release = released };

static void redraw(struct view *v)
{
	char path[] = "/tmp/ludus-XXXXXX";
	struct frame *frame;
	struct wl_shm_pool *pool;
	int fd;
	int width = v->width, height = v->height;
	if (!v->configured || !v->dirty || v->busy >= 2) return;
	if ((int64_t)width * height > INT32_MAX / 4)
		die("window too large for a Wayland shared memory buffer");
	frame = calloc(1, sizeof(*frame));
	if (!frame) die("out of memory");
	frame->owner = v;
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
	++v->busy;
	v->dirty = 0;
}

static void save_form(void)
{
	char *values[FieldCount];
	for (int i = 0; i < FieldCount; ++i) values[i] = fields[i];
	if (!*fields[Name]) {
		strcpy(form_error, "Enter a name.");
		field = Name;
		return;
	}
	if (!*fields[Exe] && !*fields[Launch]) {
		strcpy(form_error, "Enter a path or a launch command.");
		field = Exe;
		return;
	}
	for (int i = Exe; i <= Prefix; ++i) {
		if (i == Runner || !*fields[i]) continue;
		if (*fields[i] != '/') {
			strcpy(form_error, "Enter an absolute path.");
			field = i;
			return;
		}
	}
	int result = library_save(editing, values);
	if (result < 0 && errno == EEXIST) {
		field = Name;
		strcpy(form_error, "A config with this name already exists.");
	} else if (result < 0) snprintf(form_error, sizeof form_error, "Save failed: %s", strerror(errno));
	else { selected = result; close_dialog(); }
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

static void insert_text(char *value, size_t *cursor_pos, const char *str)
{
	size_t pos = *cursor_pos, len = strlen(value), n = strlen(str);
	int chars;
	int size;
	if (n >= PATH_MAX - len || !FcUtf8Len((const FcChar8 *)str, (int)n, &chars, &size)) return;
	memmove(value + pos + n, value + pos, len - pos + 1);
	memcpy(value + pos, str, n);
	*cursor_pos += n;
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
	case XKB_KEY_Tab: field = (field + 1) % FieldCount; return;
	case XKB_KEY_ISO_Left_Tab: field = (field + FieldCount - 1) % FieldCount; return;
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
		if (FcStrStrIgnoreCase((FcChar8 *)games[i].value[Name], (FcChar8 *)query)) {
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
			snprintf(status, sizeof status, "Stopped %s", games[i].value[Name]);
			if (WIFEXITED(result) && WEXITSTATUS(result))
				snprintf(status, sizeof status, "Stopped %s (exit %d)", games[i].value[Name], WEXITSTATUS(result));
			main_view.dirty = 1;
			break;
		}
	}
}

static void press(xkb_keysym_t sym)
{
	int col = columns(), old = selected;
	if (sym == KEY_QUIT) running = 0;
	if (sym == KEY_ADD || sym == XKB_KEY_KP_Add) { open_dialog(-1); return; }
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
	if (sym == KEY_EDIT) { open_dialog(selected); return; }
	if (sym == KEY_LEFT && selected % col > 0) --selected;
	if (sym == KEY_RIGHT && selected % col < col - 1 && selected + 1 < count) ++selected;
	if (sym == KEY_UP && selected >= col) selected -= col;
	if (sym == KEY_DOWN && selected / col < (count - 1) / col) {
		selected += col;
		if (selected >= count) selected = count - 1;
	}
	if (sym == KEY_LAUNCH) {
		struct game *g = &games[selected];
		const char *program;
		int error = game_launch(g, &program);
		if (error == EALREADY || !error)
			snprintf(status, sizeof status, "Playing %s", g->value[Name]);
		else {
			if (program && *program)
				snprintf(status, sizeof status, "Cannot start %s: %s", program, strerror(error));
			else
				snprintf(status, sizeof status, "No launch command or executable configured");
			fprintf(stderr, "ludus: %s: %s\n", g->value[Name], status);
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
	if (keys) xkb_state_update_mask(keys, depressed, latched, locked, 0, 0, group);
}
static void key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
                uint32_t code, uint32_t state)
{
	(void)data; (void)kb; (void)serial; (void)time;
	if (!keys || state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(keys, code + 8);
	int in_form = dialog.surface && focus == dialog.surface;
	if ((in_form || (searching && focus == main_view.surface)) &&
	    ((sym == XKB_KEY_v && xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) ||
	     (sym == XKB_KEY_Insert && xkb_state_mod_name_is_active(keys, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0))) {
		paste_target = in_form ? (int)field : FieldCount;
		clipboard_receive();
		return;
	}
	if (dialog.surface) {
		if (focus == dialog.surface) form_key(sym, code + 8);
	} else if (focus == main_view.surface) {
		if (searching) search_key(sym, code + 8);
		else press(sym);
	}
}
static void enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w, struct wl_array *a)
{ (void)d; (void)k; (void)s; focus = w; (void)a; }
static void leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w)
{ (void)d; (void)k; (void)s; if (focus == w) focus = NULL; }
static const struct wl_keyboard_listener keyboard_events = {
	.keymap = keymap, .enter = enter, .leave = leave, .key = key, .modifiers = modifiers
};

static void capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_events, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard) {
		wl_keyboard_destroy(keyboard);
		keyboard = NULL;
	}
}
static const struct wl_seat_listener seat_events = { .capabilities = capabilities };

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
		seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
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

/* Represent a CLI entry as editable shell words without losing argv boundaries. */
static int command_text(char **argv)
{
	char *out = fields[Launch], *end = out + sizeof fields[Launch] - 1;
	for (; *argv; ++argv) {
		if (out != fields[Launch]) {
			if (out == end) return -1;
			*out++ = ' ';
		}
		if (out == end) return -1;
		*out++ = '\'';
		for (const char *p = *argv; *p; ++p) {
			if (*p == '\'') {
				if (end - out < 4) return -1;
				memcpy(out, "'\\''", 4);
				out += 4;
			} else {
				if (out == end || *p == '\n') return -1;
				*out++ = *p;
			}
		}
		if (out == end) return -1;
		*out++ = '\'';
	}
	*out = 0;
	return 0;
}

static void open_dialog(int index)
{
	editing = index;
	memset(fields, 0, sizeof fields);
	if (index >= 0) {
		struct game *g = &games[index];
		for (int i = 0; i < FieldCount; ++i) {
			if (strlen(g->value[i]) >= sizeof fields[i] || strchr(g->value[i], '\n')) goto uneditable;
			strcpy(fields[i], g->value[i]);
		}
		if (*fields[Art] && fields[Art][0] != '/') {
			char cwd[PATH_MAX];
			if (!getcwd(cwd, sizeof cwd) ||
			    snprintf(fields[Art], PATH_MAX, "%s/%s", cwd, g->value[Art]) >= PATH_MAX)
				goto uneditable;
		}
		if (g->command && command_text(g->command)) goto uneditable;
	}
	for (int i = 0; i < FieldCount; ++i) cursor[i] = strlen(fields[i]);
	field = Name;
	form_error[0] = 0;
	dialog.width = 520;
	dialog.height = DialogHeight;
	create_view(&dialog, index < 0 ? "Add game" : "Edit game");
	xdg_toplevel_set_parent(dialog.window, main_view.window);
	xdg_toplevel_set_min_size(dialog.window, 420, DialogHeight);
	xdg_toplevel_set_max_size(dialog.window, 520, DialogHeight);
	wl_surface_commit(dialog.surface);
	return;
uneditable:
	strcpy(status, "Entry exceeds the single-line field limits");
	main_view.dirty = 1;
}

static void close_dialog(void)
{
	clipboard_cancel();
	if (focus == dialog.surface) focus = NULL;
	xdg_toplevel_destroy(dialog.window);
	xdg_surface_destroy(dialog.shell);
	wl_surface_destroy(dialog.surface);
	dialog.surface = NULL;
	dialog.configured = 0;
	main_view.dirty = 1;
}

static void event_loop(void)
{
	struct pollfd fds[2] = {{.fd = wl_display_get_fd(display)}, {.events = POLLIN}};
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
		if (poll(fds, 2, active ? 150 : -1) < 0) {
			if (errno == EINTR) continue;
			die(strerror(errno));
		}
		if (fds[1].revents) {
			const char *s = clipboard_read();
			if (s && paste_target == FieldCount && searching) {
				insert_text(query, &query_cursor, s);
				found = find_title(search_origin, 1);
				main_view.dirty = 1;
			} else if (s && dialog.surface && paste_target < FieldCount) {
				insert_text(fields[paste_target], &cursor[paste_target], s);
				dialog.dirty = 1;
			}
		}
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) die("Wayland connection lost");
		if ((fds[0].revents & POLLIN) && wl_display_dispatch(display) < 0) die("Wayland connection lost");
	}
}

int main(int argc, char **argv)
{
	struct wl_registry *registry;
	int i;
	font_init(FONT_PATTERN);
	library_load();
	for (i = 1; i < argc;) {
		char *values[FieldCount] = {0};
		if (!strcmp(argv[i], "--")) die("expected a cover path before --");
		values[Art] = argv[i++];
		char *base = strrchr(values[Art], '/');
		values[Name] = base ? base + 1 : values[Art];
		int index = library_append(values);
		char *dot = strrchr(games[index].value[Name], '.');
		if (dot && dot != games[index].value[Name]) *dot = 0;
		if (i < argc && strcmp(argv[i], "--")) games[index].command = argv + i;
		while (i < argc && strcmp(argv[i], "--")) ++i;
		if (i < argc) {
			argv[i++] = NULL;
			if (i == argc) die("expected a cover path after --");
		}
	}
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
	wl_display_disconnect(display);
	return 0;
}

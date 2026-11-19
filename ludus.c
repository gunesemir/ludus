#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
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

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#if COVER_WIDTH < 2 || COVER_WIDTH > 4096 || SPACING < 4 || SPACING > 1024
#error "Use a cover width of 2..4096 and spacing of 4..1024."
#endif

enum { CoverHeight = COVER_WIDTH * 3 / 2, Header = 52 };
struct game {
	const char *cover;
	char **command;
	uint32_t *thumbnail;
	int tried;
};
struct frame {
	struct wl_buffer *buffer;
	uint32_t *pixels;
	size_t bytes;
};
static struct game *games;
static int count, selected, first_row;
static int width = 900, height = 640, configured, running = 1, dirty = 1, busy;
static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static uint32_t seat_name;
static struct wl_keyboard *keyboard;
static struct wl_surface *surface;
static struct xdg_wm_base *wm;
static struct xkb_context *xkb;
static struct xkb_state *keys;
extern char **environ;

static void die(const char *message)
{
	fprintf(stderr, "ludus: %s\n", message);
	exit(EXIT_FAILURE);
}

/* Decode on first use and retain only the display-sized center crop. */
static void load_cover(struct game *g)
{
	int w, h, channels, sw, sh, x, y;
	unsigned char *src;
	g->tried = 1;
	src = stbi_load(g->cover, &w, &h, &channels, 3);
	if (!src) {
		fprintf(stderr, "ludus: %s: %s\n", g->cover, stbi_failure_reason());
		return;
	}
	g->thumbnail = malloc((size_t)COVER_WIDTH * CoverHeight * sizeof(uint32_t));
	if (!g->thumbnail)
		die("out of memory");
	sw = w;
	sh = h;
	if ((int64_t)w * CoverHeight > (int64_t)h * COVER_WIDTH)
		sw = h * COVER_WIDTH / CoverHeight;
	else
		sh = w * CoverHeight / COVER_WIDTH;
	for (y = 0; y < CoverHeight; ++y) {
		int sy = (h - sh) / 2 + y * sh / CoverHeight;
		for (x = 0; x < COVER_WIDTH; ++x) {
			int sx = (w - sw) / 2 + x * sw / COVER_WIDTH;
			const unsigned char *p = src + 3 * ((size_t)sy * w + sx);
			g->thumbnail[y * COVER_WIDTH + x] = p[0] << 16 | p[1] << 8 | p[2];
		}
	}
	stbi_image_free(src);
}

static void rect(uint32_t *pixels, int x, int y, int w, int h, uint32_t color)
{
	int i, j;
	for (j = y; j < y + h && j < height; ++j)
		for (i = x; i < x + w && i < width; ++i)
			pixels[(size_t)j * width + i] = color;
}

static int columns(void)
{
	int n = (width - SPACING) / (COVER_WIDTH + SPACING);
	return n > 0 ? n : 1;
}

static void paint(uint32_t *pixels)
{
	/* Five lowercase bitmap glyphs spell the fixed wordmark. */
	static const unsigned char logo[5][7] = {
		{12,4,4,4,4,4,14}, {0,0,17,17,17,19,13}, {1,1,13,19,17,19,13},
		{0,0,17,17,17,19,13}, {0,0,15,16,14,1,30}
	};
	int i, x, y, cols = columns();
	int rows = (height - Header - SPACING) / (CoverHeight + SPACING);
	if (rows < 1) rows = 1;
	if (first_row > selected / cols) first_row = selected / cols;
	if (first_row <= selected / cols - rows) first_row = selected / cols - rows + 1;
	rect(pixels, 0, 0, width, height, 0x161719);
	for (i = 0; i < 5; ++i)
		for (y = 0; y < 7; ++y)
			for (x = 0; x < 5; ++x)
				if (logo[i][y] & (16 >> x))
					rect(pixels, SPACING + i * 18 + x * 3, 16 + y * 3, 3, 3, 0xf0ece5);
	rect(pixels, 0, Header - 1, width, 1, 0x37383a);
	for (i = first_row * cols; i < count; ++i) {
		int left = SPACING + (i % cols) * (COVER_WIDTH + SPACING);
		int top = Header + SPACING + (i / cols - first_row) * (CoverHeight + SPACING);
		int visible = width - left;
		if (top >= height) break;
		if (visible <= 0) continue;
		if (visible > COVER_WIDTH) visible = COVER_WIDTH;
		if (i == selected)
			rect(pixels, left - 2, top - 2, COVER_WIDTH + 4, CoverHeight + 4, BORDER_COLOR);
		if (!games[i].tried) load_cover(&games[i]);
		if (!games[i].thumbnail) {
			rect(pixels, left, top, COVER_WIDTH, CoverHeight, 0x38393b);
			continue;
		}
		for (y = 0; y < CoverHeight && top + y < height; ++y)
			memcpy(pixels + (size_t)(top + y) * width + left,
			       games[i].thumbnail + y * COVER_WIDTH, visible * sizeof(*pixels));
	}
}

static void released(void *data, struct wl_buffer *buffer)
{
	struct frame *frame = data;
	wl_buffer_destroy(buffer);
	munmap(frame->pixels, frame->bytes);
	free(frame);
	--busy;
}
static const struct wl_buffer_listener buffer_events = { .release = released };

static void redraw(void)
{
	char path[] = "/tmp/ludus-XXXXXX";
	struct frame *frame;
	struct wl_shm_pool *pool;
	int fd;
	if (!configured || !dirty || busy >= 2) return;
	if ((int64_t)width * height > INT32_MAX / 4)
		die("window too large for a Wayland shared memory buffer");
	frame = calloc(1, sizeof(*frame));
	if (!frame) die("out of memory");
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
	paint(frame->pixels);
	wl_surface_attach(surface, frame->buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	++busy;
	dirty = 0;
}

static void press(xkb_keysym_t sym)
{
	int col = columns(), old = selected;
	if (sym == KEY_QUIT) running = 0;
	if (!count) return;
	if (sym == KEY_LEFT && selected % col > 0) --selected;
	if (sym == KEY_RIGHT && selected % col < col - 1 && selected + 1 < count) ++selected;
	if (sym == KEY_UP && selected >= col) selected -= col;
	if (sym == KEY_DOWN && selected / col < (count - 1) / col) {
		selected += col;
		if (selected >= count) selected = count - 1;
	}
	if (sym == KEY_LAUNCH && games[selected].command) {
		char **argv = games[selected].command;
		pid_t pid;
		int error = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
		if (error) fprintf(stderr, "ludus: %s: %s\n", argv[0], strerror(error));
	}
	if (selected != old) dirty = 1;
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
	if (keys && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		press(xkb_state_key_get_one_sym(keys, code + 8));
}
static void enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w, struct wl_array *a)
{ (void)d; (void)k; (void)s; (void)w; (void)a; }
static void leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *w)
{ (void)d; (void)k; (void)s; (void)w; }
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
	} else if (!seat && !strcmp(name, "wl_seat")) {
		seat = wl_registry_bind(r, id, &wl_seat_interface, 1);
		seat_name = id;
		wl_seat_add_listener(seat, &seat_events, NULL);
	}
}
static void removed(void *data, struct wl_registry *r, uint32_t id)
{
	(void)data; (void)r;
	if (seat && id == seat_name) {
		capabilities(NULL, seat, 0);
		wl_seat_destroy(seat);
		seat = NULL;
	}
}
static const struct wl_registry_listener registry_events = { .global = global, .global_remove = removed };

static void configure(void *data, struct xdg_surface *s, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(s, serial);
	configured = dirty = 1;
}
static const struct xdg_surface_listener surface_events = { .configure = configure };

static void resize(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
	(void)data; (void)t; (void)states;
	if (w > 0) width = w;
	if (h > 0) height = h;
}
static void closed(void *data, struct xdg_toplevel *t)
{ (void)data; (void)t; running = 0; }
static const struct xdg_toplevel_listener window_events = { .configure = resize, .close = closed };

int main(int argc, char **argv)
{
	struct wl_registry *registry;
	struct xdg_surface *shell_surface;
	struct xdg_toplevel *window;
	struct sigaction action = {0};
	int i;
	games = calloc(argc, sizeof(*games));
	if (!games) die("out of memory");
	for (i = 1; i < argc;) {
		if (!strcmp(argv[i], "--")) die("expected a cover path before --");
		games[count].cover = argv[i++];
		if (i < argc && strcmp(argv[i], "--")) games[count].command = argv + i;
		while (i < argc && strcmp(argv[i], "--")) ++i;
		if (i < argc) {
			argv[i++] = NULL;
			if (i == argc) die("expected a cover path after --");
		}
		++count;
	}
	action.sa_flags = SA_NOCLDWAIT;
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGCHLD, &action, NULL) < 0) die(strerror(errno));
	display = wl_display_connect(NULL);
	if (!display) die("cannot connect to Wayland display");
	xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb) die("cannot initialize xkbcommon");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_events, NULL);
	if (wl_display_roundtrip(display) < 0) die("cannot read Wayland registry");
	if (!compositor || !shm || !wm) die("compositor lacks wl_compositor, wl_shm or xdg-shell");
	surface = wl_compositor_create_surface(compositor);
	shell_surface = xdg_wm_base_get_xdg_surface(wm, surface);
	xdg_surface_add_listener(shell_surface, &surface_events, NULL);
	window = xdg_surface_get_toplevel(shell_surface);
	xdg_toplevel_add_listener(window, &window_events, NULL);
	xdg_toplevel_set_title(window, "ludus");
	xdg_toplevel_set_app_id(window, "ludus");
	xdg_toplevel_set_min_size(window, COVER_WIDTH + 2 * SPACING, Header + CoverHeight + 2 * SPACING);
	wl_surface_commit(surface);
	while (running) {
		if (wl_display_dispatch(display) < 0) die("Wayland connection lost");
		if (running) redraw();
	}
	wl_display_disconnect(display);
	return 0;
}

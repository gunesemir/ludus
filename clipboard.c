#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "clipboard.h"

static struct wl_data_device_manager *manager;
static struct wl_data_device *device;
static struct wl_seat *current_seat;
static struct wl_data_offer *selection, *drag;
static int input = -1;
static char buffer[PATH_MAX];
static size_t used;

static void offered_type(void *data, struct wl_data_offer *offer, const char *type)
{
	(void)data;
	/* Keep the exact offered spelling for the receive request. */
	const char *old = wl_data_offer_get_user_data(offer);
	if (!strcasecmp(type, "text/plain;charset=utf-8") ||
	    (!old && (!strcmp(type, "text/plain") || !strcmp(type, "UTF8_STRING")))) {
		char *mime = strdup(type);
		if (mime) { free((void *)old); wl_data_offer_set_user_data(offer, mime); }
	}
}
static const struct wl_data_offer_listener offer_events = { .offer = offered_type };

static void discard(struct wl_data_offer **offer)
{
	if (!*offer) return;
	free(wl_data_offer_get_user_data(*offer));
	wl_data_offer_destroy(*offer);
	*offer = NULL;
}
static void offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{ (void)d; (void)dev; wl_data_offer_add_listener(o, &offer_events, NULL); }
static void select_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{ (void)d; (void)dev; discard(&selection); selection = o; }
static void enter(void *d, struct wl_data_device *dev, uint32_t serial, struct wl_surface *s,
                  wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{ (void)d; (void)dev; (void)serial; (void)s; (void)x; (void)y; discard(&drag); drag = o; }
static void leave(void *d, struct wl_data_device *dev)
{ (void)d; (void)dev; discard(&drag); }
static void motion(void *d, struct wl_data_device *dev, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{ (void)d; (void)dev; (void)time; (void)x; (void)y; }
static const struct wl_data_device_listener events = {
	.data_offer = offer, .selection = select_offer, .enter = enter, .leave = leave,
	.motion = motion, .drop = leave
};

void clipboard_seat(struct wl_seat *seat)
{
	if (device) { wl_data_device_destroy(device); device = NULL; }
	discard(&drag);
	discard(&selection);
	clipboard_cancel();
	current_seat = seat;
	if (manager && seat) {
		device = wl_data_device_manager_get_data_device(manager, seat);
		wl_data_device_add_listener(device, &events, NULL);
	}
}
void clipboard_bind(struct wl_registry *registry, uint32_t id)
{
	manager = wl_registry_bind(registry, id, &wl_data_device_manager_interface, 1);
	clipboard_seat(current_seat);
}

void clipboard_cancel(void)
{
	if (input >= 0) close(input);
	input = -1;
}
int clipboard_fd(void) { return input; }

int clipboard_receive(void)
{
	int fds[2];
	clipboard_cancel();
	char *mime = selection ? wl_data_offer_get_user_data(selection) : NULL;
	if (!mime || pipe2(fds, O_CLOEXEC)) return 0;
	if (fcntl(fds[0], F_SETFL, O_NONBLOCK) < 0) { close(fds[0]); close(fds[1]); return 0; }
	wl_data_offer_receive(selection, mime, fds[1]);
	close(fds[1]);
	input = fds[0];
	used = 0;
	return 1;
}

/* Collect a single line. Never block the Wayland event loop on another client. */
const char *clipboard_read(void)
{
	while (input >= 0 && used < sizeof buffer) {
		ssize_t n = read(input, buffer + used, sizeof buffer - used);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && errno == EAGAIN) return NULL;
		if (n < 0) break;
		if (!n) goto finished;
		size_t end = used + (size_t)n;
		while (used < end) {
			unsigned char c = buffer[used];
			if (c == '\n' || c == '\r' || !c) goto finished;
			if (c < 32 || c == 127) { clipboard_cancel(); return NULL; }
			++used;
		}
	}
	clipboard_cancel();
	return NULL;
finished:
	buffer[used] = 0;
	clipboard_cancel();
	return buffer;
}

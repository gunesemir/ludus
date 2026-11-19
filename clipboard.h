#ifndef LUDUS_CLIPBOARD_H
#define LUDUS_CLIPBOARD_H
#include <wayland-client.h>
void clipboard_bind(struct wl_registry *registry, uint32_t id);
void clipboard_seat(struct wl_seat *seat);
int clipboard_receive(void);
int clipboard_fd(void);
void clipboard_finish(void);
void clipboard_cancel(void);
const char *clipboard_read(void);
#endif

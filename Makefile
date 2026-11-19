CC = cc
CFLAGS = -O2 -Wall -Wextra -pedantic
PKG_CONFIG ?= pkg-config
SCANNER ?= wayland-scanner
WAYLAND_CFLAGS = $(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon fontconfig freetype2)
WAYLAND_LIBS = $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon fontconfig freetype2)
PROTOCOL = $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml

ludus: build/draw.o build/library.o build/ludus.o build/xdg-shell.o
	$(CC) $(LDFLAGS) -o $@ $^ $(WAYLAND_LIBS) $(LDLIBS)

build/ludus.o: ludus.c draw.h library.h config.h vendor/stb_image.h build/xdg-shell-client-protocol.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 $(WAYLAND_CFLAGS) -Ibuild -c $< -o $@

build/draw.o: draw.c draw.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 $(WAYLAND_CFLAGS) -c $< -o $@

build/library.o: library.c library.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 -c $< -o $@

build/xdg-shell.o: build/xdg-shell.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c99 $(WAYLAND_CFLAGS) -c $< -o $@

build/xdg-shell-client-protocol.h: $(PROTOCOL) | build
	$(SCANNER) client-header $< $@

build/xdg-shell.c: $(PROTOCOL) | build
	$(SCANNER) private-code $< $@

build:
	mkdir -p $@

clean:
	rm -rf build ludus

.PHONY: clean

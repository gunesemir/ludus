#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "library.h"
#include "config.h"
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

enum { CoverHeight = COVER_WIDTH * 3 / 2 };
struct stamp { int64_t size, seconds, nanos; };

/* Validate the open descriptor so a replaced pathname cannot block the UI. */
static FILE *open_regular(const char *path, struct stat *st)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) return NULL;
	int error = 0;
	if (fstat(fd, st) < 0) error = errno;
	else if (!S_ISREG(st->st_mode)) error = EINVAL;
	else {
		FILE *f = fdopen(fd, "rb");
		if (f) return f;
		error = errno;
	}
	close(fd);
	errno = error;
	return NULL;
}

static int cache_path(const struct stat *st, char *path)
{
	const char *home = getenv("HOME");
	char dir[PATH_MAX];
	if (!home || *home != '/') return 0;
	if (snprintf(dir, sizeof dir, "%s/.cache", home) >= (int)sizeof dir ||
	    (mkdir(dir, 0700) && errno != EEXIST)) return 0;
	if (strlen(dir) + sizeof "/ludus" > sizeof dir) return 0;
	strcat(dir, "/ludus");
	if (mkdir(dir, 0700) && errno != EEXIST) return 0;
	if (snprintf(path, PATH_MAX, "%s/v2-%jx-%jx-%u-%06x.rgb", dir, (uintmax_t)st->st_dev,
	             (uintmax_t)st->st_ino, (unsigned)COVER_WIDTH, (unsigned)BACKGROUND) >= PATH_MAX) {
		*path = 0;
		return 0;
	}
	return 1;
}

static void save_cache(const char *path, const struct stamp *stamp, const void *pixels, size_t bytes)
{
	char temp[PATH_MAX];
	if (snprintf(temp, sizeof temp, "%s.XXXXXX", path) >= (int)sizeof temp) return;
	int fd = mkstemp(temp);
	if (fd < 0) return;
	FILE *f = fdopen(fd, "wb");
	if (f) {
		int ok = fwrite(stamp, sizeof *stamp, 1, f) == 1 && fwrite(pixels, 1, bytes, f) == bytes;
		if (!fclose(f) && ok && !rename(temp, path)) return;
	} else close(fd);
	unlink(temp);
}

/* Decode on first use and retain only the display-sized center crop. */
void load_cover(struct game *g)
{
	int w, h, channels, sw, sh, x, y;
	unsigned char *src;
	g->tried = 1;
	if (!*g->art) return;
	struct stat st;
	FILE *source = open_regular(g->art, &st);
	if (!source) { perror(g->art); return; }
	struct stamp stamp = {st.st_size, st.st_ctim.tv_sec, st.st_ctim.tv_nsec};
	char cache[PATH_MAX] = "";
	size_t bytes = (size_t)COVER_WIDTH * CoverHeight * sizeof(uint32_t);
	if (cache_path(&st, cache)) {
		FILE *f = open_regular(cache, &st);
		if (f) {
			struct stamp stored;
			if (fread(&stored, sizeof stored, 1, f) == 1 && !memcmp(&stored, &stamp, sizeof stamp)) {
				g->thumbnail = malloc(bytes);
				if (g->thumbnail && fread(g->thumbnail, 1, bytes, f) == bytes && fgetc(f) == EOF) {
					fclose(f);
					fclose(source);
					return;
				}
				free(g->thumbnail);
				g->thumbnail = NULL;
			}
			fclose(f);
		}
	}
	src = stbi_load_from_file(source, &w, &h, &channels, 4);
	fclose(source);
	if (!src) {
		fprintf(stderr, "ludus: %s: %s\n", g->art, stbi_failure_reason());
		return;
	}
	g->thumbnail = malloc(bytes);
	if (!g->thumbnail) {
		stbi_image_free(src);
		return;
	}
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
			const unsigned char *p = src + 4 * ((size_t)sy * w + sx);
			uint32_t pixel = 0;
			for (int c = 0; c < 3; ++c) {
				int shift = 16 - c * 8;
				unsigned bg = (BACKGROUND >> shift) & 255;
				unsigned channel = (p[c] * p[3] + bg * (255 - p[3]) + 127) / 255;
				pixel |= channel << shift;
			}
			g->thumbnail[y * COVER_WIDTH + x] = pixel;
		}
	}
	stbi_image_free(src);
	if (*cache) save_cache(cache, &stamp, g->thumbnail, bytes);
}

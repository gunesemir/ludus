#define _POSIX_C_SOURCE 200809L
#include <errno.h>
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

static int cache_path(const char *art, char *path, struct stamp *stamp)
{
	struct stat st;
	const char *home = getenv("HOME");
	char dir[PATH_MAX];
	if (!home || *home != '/' || stat(art, &st) || !S_ISREG(st.st_mode)) return 0;
	if (snprintf(dir, sizeof dir, "%s/.ludus", home) >= (int)sizeof dir ||
	    (mkdir(dir, 0700) && errno != EEXIST)) return 0;
	if (strlen(dir) + sizeof "/cache" > sizeof dir) return 0;
	strcat(dir, "/cache");
	if (mkdir(dir, 0700) && errno != EEXIST) return 0;
	if (snprintf(path, PATH_MAX, "%s/v1-%jx-%jx-%u.rgb", dir, (uintmax_t)st.st_dev,
	             (uintmax_t)st.st_ino, (unsigned)COVER_WIDTH) >= PATH_MAX) {
		*path = 0;
		return 0;
	}
	*stamp = (struct stamp){st.st_size, st.st_ctim.tv_sec, st.st_ctim.tv_nsec};
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
	if (!*g->value[Art]) return;
	struct stamp stamp;
	char cache[PATH_MAX] = "";
	size_t bytes = (size_t)COVER_WIDTH * CoverHeight * sizeof(uint32_t);
	if (cache_path(g->value[Art], cache, &stamp)) {
		FILE *f = fopen(cache, "rb");
		if (f) {
			struct stamp stored;
			if (fread(&stored, sizeof stored, 1, f) == 1 && !memcmp(&stored, &stamp, sizeof stamp)) {
				g->thumbnail = malloc(bytes);
				if (g->thumbnail && fread(g->thumbnail, 1, bytes, f) == bytes && fgetc(f) == EOF) {
					fclose(f);
					return;
				}
				free(g->thumbnail);
				g->thumbnail = NULL;
			}
			fclose(f);
		}
	}
	src = stbi_load(g->value[Art], &w, &h, &channels, 3);
	if (!src) {
		fprintf(stderr, "ludus: %s: %s\n", g->value[Art], stbi_failure_reason());
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
			const unsigned char *p = src + 3 * ((size_t)sy * w + sx);
			g->thumbnail[y * COVER_WIDTH + x] = p[0] << 16 | p[1] << 8 | p[2];
		}
	}
	stbi_image_free(src);
	if (*cache) save_cache(cache, &stamp, g->thumbnail, bytes);
}

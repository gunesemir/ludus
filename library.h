#ifndef LUDUS_LIBRARY_H
#define LUDUS_LIBRARY_H
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

struct game {
	char *path, *art, *log;
	const char *name; /* Basename of path */
	struct stat revision;
	uint32_t *thumbnail;
	pid_t pid;
	int tried;
};
extern struct game *games;
extern int count;
void load_cover(struct game *g);
int library_load(int selected, const char *preferred);
void library_finish(void);
int library_draft(const char *name, char *path, struct stat *revision);
void library_discard_draft(const char *path, const struct stat *revision);
int library_rename(const struct game *g, const char *name, char *path);
int library_remove(const struct game *g);
int game_launch(struct game *g);
void game_error(const struct game *g, char *message, size_t capacity);
#endif

#ifndef LUDUS_LIBRARY_H
#define LUDUS_LIBRARY_H
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

struct game {
	char *path, *art;
	const char *name; /* Basename of path */
	struct stat revision;
	uint32_t *thumbnail;
	pid_t pid;
	int tried;
};
extern struct game *games;
extern int count;
void load_cover(struct game *g);
void library_load(void);
void library_finish(void);
int library_refresh(struct game *g);
int library_save(struct game *g, const char *name, const char *art);
int game_launch(struct game *g);
#endif

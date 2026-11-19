#ifndef LUDUS_LIBRARY_H
#define LUDUS_LIBRARY_H
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

enum field { Name, Exe, Workdir, Art, Runner, Prefix, Proton, Launch, FieldCount };
struct game {
	char *value[FieldCount];
	char *path; /* File backing a saved entry */
	struct stat revision;
	char **command; /* Session-only command line entry */
	uint32_t *thumbnail;
	pid_t pid;
	int tried;
};
extern struct game *games;
extern int count;
int library_append(char *const values[FieldCount]);
void load_cover(struct game *g);
void library_load(void);
void library_finish(void);
int library_refresh(int index);
int library_save(int index, char *const values[FieldCount]);
int game_launch(struct game *g, const char **program);
#endif

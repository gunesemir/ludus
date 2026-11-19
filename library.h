#ifndef LUDUS_LIBRARY_H
#define LUDUS_LIBRARY_H
#include <stdint.h>
#include <sys/types.h>

enum field { Name, Exe, Art, Runner, Prefix, Proton, Launch, FieldCount };
struct game {
	char *value[FieldCount];
	char *path; /* File backing a saved entry */
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
int library_save(int index, char *const values[FieldCount]);
int game_launch(struct game *g, const char **program);
#endif

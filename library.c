#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "library.h"

struct game *games;
int count;
extern char **environ;

static char *copy(const char *value)
{
	char *s = strdup(value);
	if (!s) { perror("ludus: strdup"); exit(EXIT_FAILURE); }
	return s;
}

static char dir[PATH_MAX];

static int join(char *dst, const char *a, const char *b)
{
	if (snprintf(dst, PATH_MAX, "%s/%s", a, b) >= PATH_MAX) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

static int valid_name(const char *name)
{
	size_t len = strlen(name);
	return len && name[0] != '.' && !strchr(name, '/') &&
	       (len < 5 || strcmp(name + len - 5, ".meta"));
}

static int same_revision(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

static FILE *open_config(const char *path, struct stat *revision)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0) return NULL;
	int error = 0;
	if (fstat(fd, revision)) error = errno;
	else if (!S_ISREG(revision->st_mode)) error = ESTALE;
	else {
		FILE *f = fdopen(fd, "r");
		if (f) return f;
		error = errno;
	}
	close(fd);
	errno = error;
	return NULL;
}

/* Recognize an oyna file by its required keys. Track only the artwork value. */
static int read_oyna(FILE *f, char **art)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int valid_text = 1;
	int exe = 0, prefix = 0, proton = 0;
	while ((n = getline(&line, &cap, f)) >= 0) {
		if (memchr(line, 0, n)) { valid_text = 0; break; }
		if (n && line[n-1] == '\n') line[--n] = 0;
		if (n && line[n-1] == '\r') line[--n] = 0;
		if (!strncmp(line, "exe=", 4)) exe = line[4] != 0;
		else if (!strncmp(line, "WINEPREFIX=", 11)) prefix = line[11] != 0;
		else if (!strncmp(line, "PROTONPATH=", 11)) proton = line[11] != 0;
		else if (!strncmp(line, "art=", 4)) {
			free(*art);
			*art = copy(line + 4);
		}
	}
	free(line);
	return valid_text && exe && prefix && proton && feof(f);
}

/* Both snapshots refer to the descriptor from which we parsed the entry. */
static char *load_art(const char *path, struct stat *revision)
{
	struct stat after;
	FILE *f = open_config(path, revision);
	if (!f) return NULL;
	char *art = NULL;
	int error = 0;
	if (!read_oyna(f, &art)) error = EINVAL;
	else if (fstat(fileno(f), &after)) error = errno;
	else if (!same_revision(revision, &after)) error = ESTALE;
	fclose(f);
	if (error) { free(art); errno = error; return NULL; }
	return art ? art : copy("");
}

static void read_game(const char *path)
{
	struct stat revision;
	char *art = load_art(path, &revision);
	if (!art) {
		if (errno != EINVAL) perror(path);
		return;
	}
	struct game *p = realloc(games, (size_t)(count + 1) * sizeof(*games));
	if (!p) { perror("ludus: realloc"); exit(EXIT_FAILURE); }
	games = p;
	char *file = copy(path);
	games[count++] = (struct game){.path = file, .name = strrchr(file, '/') + 1,
	                               .art = art, .revision = revision};
}

static void free_games(struct game *list, int length)
{
	for (int i = 0; i < length; ++i) {
		free(list[i].path);
		free(list[i].thumbnail);
		free(list[i].art);
	}
	free(list);
}

int library_load(int selected, const char *preferred)
{
	const char *home = getenv("HOME");
	struct dirent **entries;
	if (!home || *home != '/' || join(dir, home, ".oyna")) { dir[0] = 0; return selected; }
	int n = scandir(dir, &entries, NULL, alphasort);
	if (n < 0 && errno != ENOENT) { perror(dir); return selected; }

	struct game *old = games;
	int old_count = count;
	const char *previous = selected < old_count ? old[selected].path : "";
	int previous_index = -1, preferred_index = -1;
	games = NULL;
	count = 0;
	for (int i = 0; i < n; ++i) {
		char path[PATH_MAX];
		const char *name = entries[i]->d_name;
		struct stat st;
		if (valid_name(name) && !join(path, dir, name) &&
		    !lstat(path, &st) && S_ISREG(st.st_mode)) read_game(path);
		free(entries[i]);
	}
	if (n >= 0) free(entries);

	for (int i = 0; i < count; ++i) {
		if (!strcmp(previous, games[i].path)) previous_index = i;
		if (preferred && !strcmp(preferred, games[i].path)) preferred_index = i;
	}
	for (int j = 0; j < old_count; ++j) {
		if (!old[j].pid) continue;
		for (int i = 0; i < count; ++i) {
			struct game *g = &games[i];
			if (g->pid) continue;
			if (!strcmp(old[j].path, g->path) ||
			    (old[j].revision.st_dev == g->revision.st_dev &&
			     old[j].revision.st_ino == g->revision.st_ino)) {
				g->pid = old[j].pid;
				break;
			}
		}
	}
	free_games(old, old_count);
	if (preferred_index >= 0) return preferred_index;
	if (previous_index >= 0) return previous_index;
	return selected < count ? selected : count ? count - 1 : 0;
}

int library_draft(char *path, struct stat *revision)
{
	const char template[] =
		"# Save this file under the game title in ~/.oyna/.\n"
		"exe=\n"
		"WINEPREFIX=\n"
		"PROTONPATH=\n"
		"art=\n";
	if (!*dir) { errno = ENOENT; return -1; }
	if (mkdir(dir, 0700) && errno != EEXIST) return -1;

	int fd;
	for (unsigned i = 1; ; ++i) {
		char name[32] = "New game";
		if (i > 1) snprintf(name, sizeof name, "New game %u", i);
		if (join(path, dir, name)) return -1;
		fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd >= 0) break;
		if (errno != EEXIST) return -1;
	}
	FILE *f = fdopen(fd, "w");
	int error = 0;
	if (!f) { error = errno; close(fd); }
	else {
		if (fputs(template, f) == EOF || fflush(f) || fstat(fd, revision)) error = errno;
		if (fclose(f) && !error) error = errno;
	}
	if (error) { unlink(path); errno = error; return -1; }
	return 0;
}

void library_discard_draft(const char *path, const struct stat *revision)
{
	struct stat current;
	if (!lstat(path, &current) && same_revision(revision, &current)) unlink(path);
}

void library_finish(void)
{
	free_games(games, count);
}

int game_launch(struct game *g)
{
	if (g->pid) return EALREADY;
	char *args[] = {"oyna", "--config", g->path, NULL};
	pid_t pid;
	int error = posix_spawnp(&pid, args[0], NULL, NULL, args, environ);
	if (!error) g->pid = pid;
	return error;
}

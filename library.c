#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/file.h>
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

static int write_oyna(FILE *out, const struct game *g, const char *art)
{
	int change_art = strcmp(g->art, art);
	if (change_art && *art && fprintf(out, "art=%s\n", art) < 0) return -1;

	/* Keep runner settings, repeated arguments and comments intact. */
	struct stat revision;
	FILE *in = open_config(g->path, &revision);
	if (!in) return -1;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int error = 0;
	if (!same_revision(&g->revision, &revision)) error = ESTALE;
	while (!error && (n = getline(&line, &cap, in)) >= 0)
		if ((!change_art || strncmp(line, "art=", 4)) &&
		    fwrite(line, 1, (size_t)n, out) != (size_t)n)
			error = errno;
	if (!error && !feof(in)) error = errno;
	free(line);
	fclose(in);
	if (error) { errno = error; return -1; }
	return 0;
}

/* Publish the new entry before retiring a previous title. */
static int write_game(const char *dest, const struct game *g, const char *art, struct stat *revision)
{
	char temp[PATH_MAX];
	int fd, error = 0;
	FILE *f;
	const char *existing = g->path;
	if (join(temp, dir, ".new-XXXXXX")) return -1;
	fd = mkstemp(temp);
	if (fd < 0) return -1;
	f = fdopen(fd, "w");
	if (!f) { error = errno; close(fd); }
	else {
		if (write_oyna(f, g, art)) error = errno == ENOENT ? ESTALE : errno;
		if (!error && (fflush(f) || fstat(fd, revision))) error = errno;
		if (fclose(f)) error = errno;
	}
	if (!error) {
		struct stat current;
		if (lstat(existing, &current)) error = errno == ENOENT ? ESTALE : errno;
		else if (!same_revision(&g->revision, &current)) error = ESTALE;
	}
	if (!error && !strcmp(existing, dest)) {
		if (!rename(temp, dest)) return 0;
		error = errno;
	} else if (!error) {
		if (link(temp, dest)) error = errno;
		else if (unlink(existing)) {
			error = errno;
			unlink(dest);
		}
	}
	unlink(temp);
	if (error) { errno = error; return -1; }
	return 0;
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

void library_load(void)
{
	const char *home = getenv("HOME");
	struct dirent **entries;
	if (!home || *home != '/' || join(dir, home, ".ludus")) { dir[0] = 0; return; }
	int n = scandir(dir, &entries, NULL, alphasort);
	if (n < 0) { if (errno != ENOENT) perror(dir); return; }
	for (int i = 0; i < n; ++i) {
		char path[PATH_MAX];
		const char *name = entries[i]->d_name;
		struct stat st;
		if (valid_name(name) && !join(path, dir, name) &&
		    !lstat(path, &st) && S_ISREG(st.st_mode)) read_game(path);
		free(entries[i]);
	}
	free(entries);
}

static void replace_art(struct game *g, char *art)
{
	if (strcmp(g->art, art)) {
		free(g->thumbnail);
		g->thumbnail = NULL;
		g->tried = 0;
	}
	free(g->art);
	g->art = art;
}

int library_refresh(struct game *g)
{
	struct stat current;
	if (lstat(g->path, &current)) return -1;
	if (!S_ISREG(current.st_mode)) { errno = ESTALE; return -1; }
	if (same_revision(&g->revision, &current)) return 0;
	char *art = load_art(g->path, &current);
	if (!art) return -1;
	replace_art(g, art);
	g->revision = current;
	return 0;
}

int library_save(struct game *g, const char *name, const char *art)
{
	char dest[PATH_MAX];
	struct stat revision;
	if (!valid_name(name)) { errno = EINVAL; return -1; }
	if (join(dest, dir, name)) return -1;
	int fd = open(dir, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	int result = flock(fd, LOCK_EX);
	if (!result) result = write_game(dest, g, art, &revision);
	int error = errno;
	close(fd);
	if (result) { errno = error; return -1; }
	replace_art(g, copy(art));
	free(g->path);
	g->path = copy(dest);
	g->name = strrchr(g->path, '/') + 1;
	g->revision = revision;
	return 0;
}

void library_finish(void)
{
	for (int i = 0; i < count; ++i) {
		free(games[i].path);
		free(games[i].thumbnail);
		free(games[i].art);
	}
	free(games);
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

#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
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

int library_append(char *const values[FieldCount])
{
	struct game *p = realloc(games, (size_t)(count + 1) * sizeof(*games));
	if (!p) { perror("ludus: realloc"); exit(EXIT_FAILURE); }
	games = p;
	games[count] = (struct game){0};
	for (int i = 0; i < FieldCount; ++i)
		games[count].value[i] = copy(values[i] ? values[i] : "");
	return count++;
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

static int valid(char *const v[FieldCount])
{
	return *v[Name] && v[Exe][0] == '/' && v[Art][0] == '/' &&
	       (!strcmp(v[Runner], "wine") || !strcmp(v[Runner], "umu-run")) &&
	       (!*v[Prefix] || v[Prefix][0] == '/') && (!*v[Proton] || v[Proton][0] == '/');
}

static void read_game(const char *entry)
{
	char path[PATH_MAX], *values[FieldCount] = {0};
	FILE *f;
	int i;
	if (join(path, dir, entry) || !(f = fopen(path, "r"))) {
		perror(entry);
		return;
	}
	for (i = 0; i < FieldCount; ++i) {
		size_t cap = 0;
		ssize_t n = getline(&values[i], &cap, f);
		if (n < 1 || values[i][n-1] != '\n' || memchr(values[i], 0, n)) break;
		values[i][n-1] = 0;
	}
	if (i == FieldCount && valid(values)) library_append(values);
	else fprintf(stderr, "ludus: invalid entry: %s\n", path);
	fclose(f);
	for (i = 0; i < FieldCount; ++i) free(values[i]);
}

void library_load(void)
{
	const char *base = getenv("XDG_DATA_HOME");
	char fallback[PATH_MAX];
	struct dirent **entries;
	int n;
	if (!base || *base != '/') {
		base = getenv("HOME");
		if (!base || *base != '/' || join(fallback, base, ".local/share")) return;
		base = fallback;
	}
	if (join(dir, base, "ludus/games")) { dir[0] = 0; return; }
	n = scandir(dir, &entries, NULL, alphasort);
	if (n < 0) { if (errno != ENOENT) perror(dir); return; }
	for (int i = 0; i < n; ++i) {
		if (!strncmp(entries[i]->d_name, "game-", 5)) read_game(entries[i]->d_name);
		free(entries[i]);
	}
	free(entries);
}

static int create_dirs(void)
{
	char path[PATH_MAX];
	strcpy(path, dir);
	for (char *p = path + 1;; ++p) {
		int end = !*p;
		if (!end && *p != '/') continue;
		*p = 0;
		if (mkdir(path, 0700) && errno != EEXIST) return -1;
		if (end) return 0;
		*p = '/';
	}
}

/* Publish a complete entry with one link; parallel writers never overwrite. */
int library_save(char *const values[FieldCount])
{
	char temp[PATH_MAX], dest[PATH_MAX];
	int fd, error = 0;
	FILE *f;
	if (!valid(values)) { errno = EINVAL; return -1; }
	if (!*dir) { errno = ENOENT; return -1; }
	if (create_dirs() || join(temp, dir, ".new-XXXXXX")) return -1;
	fd = mkstemp(temp);
	if (fd < 0) return -1;
	f = fdopen(fd, "w");
	if (!f) { error = errno; close(fd); }
	else {
		for (int i = 0; i < FieldCount; ++i)
			if (fprintf(f, "%s\n", values[i]) < 0) { error = errno; break; }
		if (fclose(f)) error = errno;
	}
	if (!error) {
		strcpy(dest, temp);
		memcpy(strrchr(dest, '/') + 1, "game-", 5);
		if (link(temp, dest)) error = errno;
	}
	unlink(temp);
	if (error) { errno = error; return -1; }
	return library_append(values);
}

int game_launch(struct game *g)
{
	char *runner[] = {g->value[Runner], g->value[Exe], NULL};
	char **args = g->command ? g->command : runner;
	char *prefix = g->value[Prefix], *proton = g->value[Proton];
	char prefix_env[PATH_MAX + 12], proton_env[PATH_MAX + 12];
	char **env;
	size_t i, n = 0;
	int error;
	pid_t pid;
	if (g->pid) return EALREADY;
	if (!*args[0]) return ENOEXEC;
	if (strcmp(g->value[Runner], "umu-run")) proton = "";
	if (snprintf(prefix_env, sizeof prefix_env, "WINEPREFIX=%s", prefix) >= (int)sizeof prefix_env ||
	    snprintf(proton_env, sizeof proton_env, "PROTONPATH=%s", proton) >= (int)sizeof proton_env)
		return ENAMETOOLONG;
	for (i = 0; environ[i]; ++i) {}
	env = calloc(i + 3, sizeof *env);
	if (!env) return ENOMEM;
	for (i = 0; environ[i]; ++i) {
		if (*prefix && !strncmp(environ[i], "WINEPREFIX=", 11)) continue;
		if (*proton && !strncmp(environ[i], "PROTONPATH=", 11)) continue;
		env[n++] = environ[i];
	}
	if (*prefix) env[n++] = prefix_env;
	if (*proton) env[n++] = proton_env;
	error = posix_spawnp(&pid, args[0], NULL, NULL, args, env);
	free(env);
	if (!error) g->pid = pid;
	return error;
}

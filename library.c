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
static const char *const keys[FieldCount] = {
	"name", "exe", "workdir", "art", "runner", "WINEPREFIX", "PROTONPATH", "cmd"
};

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
	return *v[Name] && (v[Exe][0] == '/' || (!*v[Exe] && *v[Launch])) && (!*v[Art] || v[Art][0] == '/') &&
	       (!*v[Prefix] || v[Prefix][0] == '/') && (!*v[Workdir] || v[Workdir][0] == '/');
}

/* Keep titles readable while encoding filename separators and literal escapes. */
static int game_path(char *dest, const char *name)
{
	char file[NAME_MAX + 1];
	size_t n = 0;
	for (const char *p = name; *p; ++p) {
		const char *escape = NULL;
		if (*p == '/') escape = "%2F";
		if (*p == '%') escape = "%25";
		if (p == name && *p == '.') escape = "%2E";
		size_t bytes = escape ? 3 : 1;
		if (n + bytes + sizeof ".meta" > sizeof file) { errno = ENAMETOOLONG; return -1; }
		if (escape) memcpy(file + n, escape, bytes);
		else file[n] = *p;
		n += bytes;
	}
	strcpy(file + n, ".meta");
	return join(dest, dir, file);
}

static int same_revision(const struct stat *a, const struct stat *b)
{
	return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
	       a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec;
}

/* Publish the new entry before retiring a previous title. */
static int write_game(const char *dest, const struct game *old, char *const values[FieldCount], struct stat *revision)
{
	char temp[PATH_MAX];
	int fd, error = 0;
	FILE *f;
	const char *existing = old ? old->path : NULL;
	if (join(temp, dir, ".new-XXXXXX")) return -1;
	fd = mkstemp(temp);
	if (fd < 0) return -1;
	f = fdopen(fd, "w");
	if (!f) { error = errno; close(fd); }
	else {
		for (int i = 0; i < FieldCount; ++i)
			if (fprintf(f, "%s=%s\n", keys[i], values[i]) < 0) { error = errno; break; }
		if (!error && (fflush(f) || fstat(fd, revision))) error = errno;
		if (fclose(f)) error = errno;
	}
	if (!error && existing) {
		struct stat current;
		if (lstat(existing, &current)) error = errno == ENOENT ? ESTALE : errno;
		else if (!same_revision(&old->revision, &current)) error = ESTALE;
	}
	if (!error && existing && !strcmp(existing, dest)) {
		if (!rename(temp, dest)) return 0;
		error = errno;
	} else if (!error) {
		if (link(temp, dest)) error = errno;
		else if (existing && unlink(existing)) {
			error = errno;
			unlink(dest);
		}
	}
	unlink(temp);
	if (error) { errno = error; return -1; }
	return 0;
}

static int read_meta(FILE *f, char **values)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	int ok = 1;
	while ((n = getline(&line, &cap, f)) >= 0) {
		if (memchr(line, 0, n)) { ok = 0; break; }
		while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
		if (!n || line[0] == '#') continue;
		char *value = strchr(line, '=');
		if (!value) { ok = 0; break; }
		*value++ = 0;
		int i = 0;
		while (i < FieldCount && strcmp(keys[i], line)) ++i;
		if (i == FieldCount || values[i]) { ok = 0; break; }
		values[i] = copy(value);
	}
	free(line);
	for (int i = 0; i < FieldCount; ++i)
		if (!values[i]) values[i] = copy("");
	return ok && !ferror(f);
}

/* Both snapshots refer to the descriptor from which we parsed the entry. */
static int load_values(const char *path, char **values, struct stat *revision)
{
	struct stat after;
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
	if (fd < 0) return -1;
	FILE *f = fdopen(fd, "r");
	int error = 0;
	if (!f) { error = errno; close(fd); errno = error; return -1; }
	if (fstat(fd, revision)) error = errno;
	else if (!S_ISREG(revision->st_mode)) error = ESTALE;
	else if (!read_meta(f, values) || !valid(values)) error = EINVAL;
	else if (fstat(fd, &after)) error = errno;
	else if (!same_revision(revision, &after)) error = ESTALE;
	fclose(f);
	if (error) { errno = error; return -1; }
	return 0;
}

static void read_game(const char *path)
{
	char *values[FieldCount] = {0};
	struct stat revision;
	if (!load_values(path, values, &revision)) {
		int index = library_append(values);
		games[index].path = copy(path);
		games[index].revision = revision;
	} else perror(path);
	for (int i = 0; i < FieldCount; ++i) free(values[i]);
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
		size_t len = strlen(name);
		if (len > 5 && !strcmp(name + len - 5, ".meta") && !join(path, dir, name)) {
			struct stat st;
			if (!lstat(path, &st) && S_ISREG(st.st_mode)) read_game(path);
		}
		free(entries[i]);
	}
	free(entries);
}

static void replace_values(struct game *g, char *const values[FieldCount])
{
	if (strcmp(g->value[Art], values[Art])) {
		free(g->thumbnail);
		g->thumbnail = NULL;
		g->tried = 0;
	}
	for (int i = 0; i < FieldCount; ++i) {
		free(g->value[i]);
		g->value[i] = copy(values[i]);
	}
	g->command = NULL;
}

int library_refresh(int index)
{
	struct game *g = &games[index];
	struct stat current;
	if (!g->path) return 0;
	if (lstat(g->path, &current)) return -1;
	if (!S_ISREG(current.st_mode)) { errno = ESTALE; return -1; }
	if (same_revision(&g->revision, &current)) return 0;
	char *values[FieldCount] = {0};
	int result = load_values(g->path, values, &current);
	int error = errno;
	if (!result) {
		replace_values(g, values);
		g->revision = current;
	}
	for (int i = 0; i < FieldCount; ++i) free(values[i]);
	errno = error;
	return result;
}

int library_save(int index, char *const values[FieldCount])
{
	char dest[PATH_MAX];
	struct stat revision;
	if (!valid(values)) { errno = EINVAL; return -1; }
	if (!*dir) { errno = ENOENT; return -1; }
	if (game_path(dest, values[Name])) return -1;
	if (mkdir(dir, 0700) && errno != EEXIST) return -1;
	int fd = open(dir, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	int result = flock(fd, LOCK_EX);
	if (!result) result = write_game(dest, index >= 0 ? &games[index] : NULL, values, &revision);
	int error = errno;
	close(fd);
	if (result) { errno = error; return -1; }
	if (index < 0) index = library_append(values);
	else replace_values(&games[index], values);
	free(games[index].path);
	games[index].path = copy(dest);
	games[index].revision = revision;
	return index;
}

void library_finish(void)
{
	for (int i = 0; i < count; ++i) {
		free(games[i].path);
		free(games[i].thumbnail);
		for (int j = 0; j < FieldCount; ++j) free(games[i].value[j]);
	}
	free(games);
}

/* File actions run in the child, before PATH lookup and exec. */
static int spawn_game(pid_t *pid, char **args, char **env, const char *workdir)
{
	if (!*workdir) return posix_spawnp(pid, args[0], NULL, NULL, args, env);
	posix_spawn_file_actions_t actions;
	int error = posix_spawn_file_actions_init(&actions);
	if (error) return error;
	error = posix_spawn_file_actions_addchdir_np(&actions, workdir);
	if (!error) error = posix_spawnp(pid, args[0], &actions, NULL, args, env);
	posix_spawn_file_actions_destroy(&actions);
	return error;
}

int game_launch(struct game *g, const char **program)
{
	char *launch[7], **args = launch;
	size_t argc = 0;
	if (*g->value[Launch]) {
		launch[argc++] = "/bin/sh";
		launch[argc++] = "-c";
		launch[argc++] = g->value[Launch];
		launch[argc++] = "ludus";
	}
	if (*g->value[Runner]) launch[argc++] = g->value[Runner];
	if (*g->value[Exe]) launch[argc++] = g->value[Exe];
	launch[argc] = NULL;
	if (g->command) args = g->command;
	char *prefix = g->value[Prefix], *proton = g->value[Proton];
	char prefix_env[PATH_MAX + 12], proton_env[PATH_MAX + 12], pwd_env[PATH_MAX + 4];
	const char *workdir = g->value[Workdir];
	char **env;
	size_t i, n = 0;
	int error;
	pid_t pid;
	*program = args[0];
	if (g->pid) return EALREADY;
	if (!args[0] || !*args[0]) return ENOEXEC;
	if (snprintf(pwd_env, sizeof pwd_env, "PWD=%s", workdir) >= (int)sizeof pwd_env ||
	    snprintf(prefix_env, sizeof prefix_env, "WINEPREFIX=%s", prefix) >= (int)sizeof prefix_env ||
	    snprintf(proton_env, sizeof proton_env, "PROTONPATH=%s", proton) >= (int)sizeof proton_env)
		return ENAMETOOLONG;
	for (i = 0; environ[i]; ++i) {}
	env = calloc(i + 4, sizeof *env);
	if (!env) return ENOMEM;
	for (i = 0; environ[i]; ++i) {
		if (*workdir && !strncmp(environ[i], "PWD=", 4)) continue;
		if (*prefix && !strncmp(environ[i], "WINEPREFIX=", 11)) continue;
		if (*proton && !strncmp(environ[i], "PROTONPATH=", 11)) continue;
		env[n++] = environ[i];
	}
	if (*workdir) env[n++] = pwd_env;
	if (*prefix) env[n++] = prefix_env;
	if (*proton) env[n++] = proton_env;
	error = spawn_game(&pid, args, env, workdir);
	free(env);
	if (!error) g->pid = pid;
	return error;
}

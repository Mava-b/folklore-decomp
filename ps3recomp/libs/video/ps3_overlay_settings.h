/*
 * ps3recomp - the in-game menu's settings file (ps3_overlay.mm)
 *
 * The file (PS3_OVERLAY_INI=<path>) is a list of VARIABLE=value lines with the
 * same names and values as the environment variables, so it reads like a list
 * of extra variables. Before main, each one whose variable is not set is
 * exported, and every module reads it the way it always read its variable;
 * a variable that is set when the game starts wins over its line. Only the
 * settings changed in the menu are written back.
 *
 * One session, several process images. A title that replaces its executable
 * (Resistance: the EBOOT's menus exitspawn game.self, an execv) starts the
 * menu again in the new image, which inherits the environment of the old
 * one -- including what the old one exported from the file, which would then
 * look like the player's own variables and win over the file. So the first
 * image of a session records which variables the player really set
 * (PS3_OVERLAY_USERENV, a comma list) and marks the session
 * (PS3_OVERLAY_SESSION=1); just before the exec, the old image exports what
 * the menu changed (ps3os_export_session), and the new image takes the
 * environment as it finds it and reads the file only to know its lines.
 *
 * Plain C, header only: the menu, the window host (tools/host_window reads
 * the display settings from the same file, ps3os_lookup_file) and a
 * standalone test (libs/video/tests/test_overlay_settings.c) share it.
 *
 * From the inFAMOUS 2 port (its ps3_overlay.mm keeps the same file format);
 * the session handling across an exec is new here.
 */
#ifndef PS3RECOMP_PS3_OVERLAY_SETTINGS_H
#define PS3RECOMP_PS3_OVERLAY_SETTINGS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PS3OS_VAL      48     /* longest value kept */
#define PS3OS_SESSION  "PS3_OVERLAY_SESSION"
#define PS3OS_USERENV  "PS3_OVERLAY_USERENV"

/* "KEY = value" (quotes and blanks around the value dropped) -> 1, with
 * the key and the value pointing into `line`; comments (# ;), [sections] and blank lines
 * -> 0. */
static inline int ps3os_split(char* line, char** key, char** val)
{
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#' || *s == ';' || *s == '[' || !*s || *s == '\n' || *s == '\r') return 0;
    char* eq = strchr(s, '=');
    if (!eq || eq == s) return 0;
    *eq = 0;
    for (char* e = eq - 1; e >= s && (*e == ' ' || *e == '\t'); e--) *e = 0;
    char* v = eq + 1;
    while (*v == ' ' || *v == '\t' || *v == '"') v++;
    for (char* e = v + strlen(v) - 1; e >= v && (*e == '\n' || *e == '\r' || *e == ' ' ||
                                                 *e == '\t' || *e == '"'); e--) *e = 0;
    if (!*s) return 0;
    *key = s; *val = v;
    return 1;
}

/* `name` in a comma list. */
static inline int ps3os_list_has(const char* list, const char* name)
{
    const size_t n = strlen(name);
    for (const char* p = list; p && *p; ) {
        const char* c = strchr(p, ',');
        const size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        p = c ? c + 1 : NULL;
    }
    return 0;
}

/* One variable's line of the file at `path`: 1 and its value in out. */
static inline int ps3os_lookup_file(const char* path, const char* name, char* out, size_t n)
{
    if (!path || !*path) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *k, *v;
        if (ps3os_split(line, &k, &v) && !strcmp(k, name)) {
            snprintf(out, n, "%s", v);
            found = 1;                 /* the last line of a name wins, as in the menu */
        }
    }
    fclose(f);
    return found;
}

/* At start (a constructor, before any thread): which of names[0..n) the
 * player set (env_set), the file's lines (saved, "" = not in it), and, in the
 * first image of a session, each line whose variable is not set exported.
 * read_file = 0: the file is not even opened (a headless run nobody asked
 * the menu of). Returns the number of lines exported. */
static inline int ps3os_preload(const char* const* names, int n, unsigned char* env_set,
                                char (*saved)[PS3OS_VAL], const char* path, int read_file)
{
    const char* session = getenv(PS3OS_SESSION);
    const int first = !(session && *session);
    if (first) {
        char list[1024] = "";
        size_t len = 0;
        for (int i = 0; i < n; i++) {
            env_set[i] = getenv(names[i]) != NULL;
            if (!env_set[i]) continue;
            const int w = snprintf(list + len, sizeof list - len, "%s%s", len ? "," : "", names[i]);
            if (w > 0 && (size_t)w < sizeof list - len) len += (size_t)w;
        }
        setenv(PS3OS_USERENV, list, 1);
        setenv(PS3OS_SESSION, "1", 1);
    } else {
        const char* list = getenv(PS3OS_USERENV);
        for (int i = 0; i < n; i++) env_set[i] = (unsigned char)ps3os_list_has(list ? list : "", names[i]);
    }
    for (int i = 0; i < n; i++) saved[i][0] = 0;
    if (!read_file || !path || !*path) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int applied = 0;
    while (fgets(line, sizeof line, f)) {
        char *k, *v;
        if (!ps3os_split(line, &k, &v)) continue;
        for (int i = 0; i < n; i++) {
            if (strcmp(k, names[i])) continue;
            snprintf(saved[i], PS3OS_VAL, "%s", v);
            /* A later image of the session finds the earlier one's values in
             * the environment already (exported, or changed in its menu). */
            if (first && !env_set[i]) { setenv(names[i], v, 1); applied++; }
        }
    }
    fclose(f);
    return applied;
}

/* Just before an exec: what the menu changed this session (value[i], ""
 * = untouched) goes into the environment the next image starts with. */
static inline void ps3os_export_session(const char* const* names, int n, char (*value)[PS3OS_VAL])
{
    for (int i = 0; i < n; i++)
        if (value[i][0]) setenv(names[i], value[i], 1);
}

/* Write the file: its lines (saved) with the menu's changes (value) over
 * them, through a temporary file and a rename; the folder is made if need
 * be. Afterwards saved holds what was written. 1 = written. */
static inline int ps3os_save(const char* path, const char* const* names, int n,
                             char (*saved)[PS3OS_VAL], char (*value)[PS3OS_VAL], const char* header)
{
    if (!path || !*path) return 0;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    { char dir[1024]; snprintf(dir, sizeof dir, "%s", path);   /* mkdir -p of the folder */
      for (char* q = dir + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; } }
    FILE* f = fopen(tmp, "w");
    if (!f) return 0;
    if (header) fputs(header, f);
    for (int i = 0; i < n; i++) {
        const char* v = value[i][0] ? value[i] : saved[i];
        if (*v) fprintf(f, "%s=%s\n", names[i], v);
    }
    const int ok = fclose(f) == 0 && rename(tmp, path) == 0;
    if (!ok) { unlink(tmp); return 0; }
    for (int i = 0; i < n; i++)
        if (value[i][0]) snprintf(saved[i], PS3OS_VAL, "%s", value[i]);
    return 1;
}

#endif /* PS3RECOMP_PS3_OVERLAY_SETTINGS_H */

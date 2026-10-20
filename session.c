/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "session.h"
#include "platform.h"
#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define SESSION_LIMIT (32UL * 1024UL * 1024UL)
/* <state>/sessions/<folder>/<id>.json holds one conversation; <folder>/latest
 * names the most recently saved one. The folder is named after the directory. */
static char *dir, *file, *id, *cwd_path, *last; static StoreLock *lock; static int failed;
static char *join(const char *a, const char *b, const char *c) {
    char *s = xmalloc(strlen(a) + strlen(b) + strlen(c) + 2); sprintf(s, "%s/%s%s", a, b, c); return s;
}
/* The folder is the directory's path with separators as '-' and hyphens doubled:
 * /home/me/my-project -> home-me-my--project (C:\src -> C-src on Windows). Only
 * a hyphen next to a separator is ambiguous (a-/b and a/-b are both a---b); the
 * "cwd" recorded in each session catches that. Long paths keep their specific end. */
#define FOLDER_LIMIT 200
static char *session_dir(const char *cwd) {
    char *key = xmalloc(2 * strlen(cwd) + 2), *data, *s, *start; size_t k = 0; int pending = 0;
    for (; *cwd; cwd++) {
        if (*cwd == '/' || *cwd == '\\' || *cwd == ':') { pending = k > 0; continue; }
        if (pending) { key[k++] = '-'; pending = 0; }
        key[k++] = *cwd; if (*cwd == '-') key[k++] = '-';
    }
    if (!k) key[k++] = '-'; /* The root directory. */
    key[k] = 0; start = key;
    if (k > FOLDER_LIMIT) { start = key + k - FOLDER_LIMIT; while (((unsigned char)*start & 0xc0) == 0x80) start++; }
    data = host_dir(HOST_STATE); if (!data) { free(key); return NULL; }
    s = join(data, "sessions/", start); free(data); free(key); return s;
}
static int valid_id(const char *s) {
    if (!*s || strlen(s) > 40) return 0;
    for (; *s; s++) if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') || *s == '-')) return 0;
    return 1;
}
void session_end(void) {
    store_unlock(lock); lock = NULL; failed = 0;
    free(dir); free(file); free(id); free(cwd_path); free(last); dir = file = id = cwd_path = last = NULL;
}
void session_start(const char *cwd) {
    char name[48]; time_t t = time(NULL); struct tm *u = gmtime(&t);
    session_end(); dir = session_dir(cwd); if (!dir) return;
    if (!u || !strftime(name, sizeof name, "%Y%m%d-%H%M%SZ", u)) strcpy(name, "session");
    sprintf(name + strlen(name), "-%lu", host_pid() % 100000000UL);
    cwd_path = xstrdup(cwd); id = xstrdup(name); file = join(dir, id, ".json");
}
Json *session_resume(const char *cwd) {
    char *pointer, *text; Json *j; size_t n;
    session_end(); dir = session_dir(cwd);
    if (!dir) { fputs("no home directory; cannot find saved sessions\n", stderr); return NULL; }
    pointer = join(dir, "latest", ""); id = host_read_private(pointer, 64); free(pointer);
    if (id) { n = strlen(id); while (n && (id[n - 1] == '\n' || id[n - 1] == '\r')) id[--n] = 0; }
    if (!id || !valid_id(id)) { fputs("no saved hark session for this directory\n", stderr); session_end(); return NULL; }
    cwd_path = xstrdup(cwd); file = join(dir, id, ".json");
    if (!(lock = store_lock(file))) { fputs("that session is open in another hark; quit it first\n", stderr); session_end(); return NULL; }
    text = host_read_private(file, SESSION_LIMIT); j = text ? jparse(text) : NULL;
    if (j && j->type == J_OBJ && jget(j, "cwd") && jget(j, "cwd")->type == J_STR && strcmp(jget(j, "cwd")->s, cwd)) {
        /* a-/b and a/-b share a folder; see session_dir. */
        fputs("no saved hark session for this directory (the last one in its folder is for ", stderr);
        host_eprint(jget(j, "cwd")->s); fputs(")\n", stderr); jfree(j); free(text); session_end(); return NULL;
    }
    if (!j || j->type != J_OBJ || !jget(j, "cwd")) {
        fputs("saved session is missing, unreadable, or unsafe\n", stderr); jfree(j); free(text); session_end(); return NULL;
    }
    last = text; return j;
}
int session_saved(void) { return last != NULL; }
int session_save(Json *context) {
    Json *h = jget(context, "history"); char *text, *pointer, *line; int ok;
    if (!file || failed) return 0;
    if (!last && (!h || !h->child)) return 1; /* Write nothing until there is a conversation. */
    jset(context, "cwd", jstr(cwd_path)); text = jdump(context);
    if (last && !strcmp(last, text)) { free(text); return 1; }
    ok = lock || (host_mkdirs(dir) && (lock = store_lock(file)) != NULL);
    if (ok) ok = host_write_secret(file, text);
    if (ok) {
        pointer = join(dir, "latest", ""); line = xmalloc(strlen(id) + 2); sprintf(line, "%s\n", id);
        ok = host_write_secret(pointer, line); free(pointer); free(line);
    }
    if (!ok) { free(text); failed = 1; fputs("cannot save session; continuing without it\n", stderr); return 0; }
    free(last); last = text; return 1;
}

/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "store.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif
#define STORE_LIMIT (32UL * 1024UL * 1024UL)
/* The startup file is input only: Hark never writes it, or any .harkrc/harkrc. */
static char *startup_path;
#ifdef _WIN32
typedef struct { DWORD volume, high, low; } FileId;
static int file_id(const char *path, FileId *id) {
    HANDLE f = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BY_HANDLE_FILE_INFORMATION info; int ok;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = GetFileInformationByHandle(f, &info); CloseHandle(f);
    if (ok) { id->volume = info.dwVolumeSerialNumber; id->high = info.nFileIndexHigh; id->low = info.nFileIndexLow; }
    return ok;
}
static int same_id(const FileId *a, const FileId *b) { return a->volume == b->volume && a->high == b->high && a->low == b->low; }
#else
typedef struct { dev_t dev; ino_t ino; } FileId;
static int file_id(const char *path, FileId *id) {
    struct stat st; if (stat(path, &st)) return 0;
    id->dev = st.st_dev; id->ino = st.st_ino; return 1;
}
static int same_id(const FileId *a, const FileId *b) { return a->dev == b->dev && a->ino == b->ino; }
#endif
static FileId startup_id; static int have_startup_id;
static int startup_file(const char *path) {
    const char *base = path, *p; FileId id;
    for (p = path; *p; p++) if (*p == '/'
#ifdef _WIN32
        || *p == '\\' || *p == ':'
#endif
    ) base = p + 1;
    /* Case-insensitive: macOS and Windows filesystems usually are. */
    if (*base == '.') base++;
    for (p = "harkrc"; *p && tolower((unsigned char)*base) == *p; p++) base++;
    if (!*p && !*base) return 1;
    if (startup_path && !strcmp(startup_path, path)) return 1;
    return have_startup_id && file_id(path, &id) && same_id(&id, &startup_id);
}
static int exists_file(const char *path) {
#ifdef _WIN32
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
#else
    struct stat st; if (!lstat(path, &st)) return 1; return errno == ENOENT ? 0 : -1;
#endif
}
char *store_text(const char *path, size_t limit) {
    FILE *f = fopen(path, "rb"); char *s; size_t used = 0, cap = 4096, n;
    if (!f) { fprintf(stderr, "cannot open file: %s\n", path); return NULL; }
    if (cap > limit + 1) cap = limit + 1;
    s = xmalloc(cap);
    for (;;) {
        if (used + 1 == cap) {
            size_t next = cap * 2; if (next > limit + 1) next = limit + 1;
            if (next == cap) {
                if (fgetc(f) == EOF && !ferror(f)) break;
                fputs("file exceeds size limit\n", stderr); free(s); fclose(f); return NULL;
            }
            s = xrealloc(s, next); cap = next;
        }
        n = fread(s + used, 1, cap - used - 1, f); used += n;
        if (!n) break;
    }
    if (ferror(f) || memchr(s, 0, used)) { fputs("cannot read file or file contains NUL\n", stderr); free(s); s = NULL; }
    else s[used] = 0;
    fclose(f); return s;
}
/* .harkrc is hand-written, often with normal 0644 permissions. Reject symlinks,
 * nonregular files and files writable by other users before loading policy. */
static int usable(const char *path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
#else
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 022);
#endif
}
void store_close(void) { free(startup_path); startup_path = NULL; have_startup_id = 0; }
char *store_read(const char *path, int *ok) {
    int exists = exists_file(path); char *text; *ok = 0;
    store_close(); startup_path = xstrdup(path);
    if (!exists) { *ok = 1; return NULL; }
    if (exists < 0 || !usable(path) || !(text = store_text(path, STORE_LIMIT))) {
        fputs("cannot load harkrc: unreadable or unsafe file\n", stderr); return NULL;
    }
    have_startup_id = file_id(path, &startup_id);
    *ok = 1; return text;
}
#ifdef _WIN32
typedef HANDLE StateLock;
#define NO_LOCK INVALID_HANDLE_VALUE
static void close_lock(StateLock lock) { if (lock != NO_LOCK) CloseHandle(lock); }
static StateLock open_lock(const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    BY_HANDLE_FILE_INFORMATION info;
    if (f != INVALID_HANDLE_VALUE && (!GetFileInformationByHandle(f, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))) { CloseHandle(f); return NO_LOCK; }
    return f;
}
#else
typedef int StateLock;
#define NO_LOCK (-1)
static void close_lock(StateLock lock) { if (lock != NO_LOCK) close(lock); }
static StateLock open_lock(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT
#ifdef O_NOFOLLOW
                  | O_NOFOLLOW
#endif
                  , 0600);
    struct stat st; struct flock lock;
    if (fd < 0) return NO_LOCK;
    memset(&lock, 0, sizeof lock); lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || fcntl(fd, F_SETLK, &lock) < 0) { close(fd); return NO_LOCK; }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC); return fd;
}
#endif
struct StoreLock { StateLock lock; };
StoreLock *store_lock(const char *path) {
    char *lock_path = xmalloc(strlen(path) + 6); StoreLock *l; StateLock lock;
    sprintf(lock_path, "%s.lock", path); lock = open_lock(lock_path); free(lock_path);
    if (lock == NO_LOCK) return NULL;
    l = xmalloc(sizeof *l); l->lock = lock; return l;
}
void store_unlock(StoreLock *l) { if (l) { close_lock(l->lock); free(l); } }
int store_write(const char *path, const char *text) {
    StoreLock *lock; int ok = 0, exists;
    if (startup_file(path)) { fputs("refusing to write a startup file (harkrc or --rc); choose another file\n", stderr); return 0; }
    lock = store_lock(path);
    if (!lock) { fputs("cannot lock file for saving\n", stderr); return 0; }
    exists = exists_file(path);
    if (exists < 0 || (exists && !usable(path))) goto done;
    if (strlen(text) <= STORE_LIMIT) ok = host_write_secret(path, text);
done:
    store_unlock(lock);
    if (!ok) fputs("cannot save file (maximum 32 MiB; destination must be a regular owned file)\n", stderr);
    return ok;
}

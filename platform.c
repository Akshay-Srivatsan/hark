/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <direct.h>
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#endif

static void print_safe(FILE *out, const char *s) { /* Also strip UTF-8 C1 controls (CSI, OSC, ...). */
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p == 0xc2 && p[1] >= 0x80 && p[1] <= 0x9f) { p += 2; continue; }
        if (*p >= 32 && *p != 127) fputc(*p, out);
        else if (*p == '\n' || *p == '\t') fputc(*p, out);
        else fprintf(out, "\\x%02x", (unsigned)*p);
        p++;
    }
    fflush(out);
}
void host_print(const char *s) { print_safe(stdout, s); }
void host_eprint(const char *s) { print_safe(stderr, s); }
const char *host_home(void) {
    const char *s = getenv("HOME");
#ifdef _WIN32
    if (!s || !*s) s = getenv("USERPROFILE");
#endif
    return s && *s ? s : NULL;
}
static int absolute(const char *p) {
#ifdef _WIN32
    if (p[0] == '\\' || (p[0] && p[1] == ':')) return 1;
#endif
    return p[0] == '/';
}
char *host_dir(int kind) {
    static const char *const names[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"};
    static const char *const fallbacks[] = {"/.config", "/.local/share", "/.local/state", "/.cache"};
    const char *base = getenv(names[kind]), *before = "", *after = ""; char *s;
    /* The spec says relative values are invalid and must be ignored. */
    if (!base || !*base || !absolute(base)) {
        base = NULL;
#ifdef _WIN32
        base = getenv(kind == HOST_CONFIG ? "APPDATA" : "LOCALAPPDATA");
        if (base && (!*base || !absolute(base))) base = NULL;
        if (base) after = kind == HOST_STATE ? "/state" : kind == HOST_CACHE ? "/cache" : "";
#endif
        if (!base) { base = host_home(); if (!base) return NULL; before = fallbacks[kind]; }
    }
    s = xmalloc(strlen(base) + strlen(before) + strlen(after) + 6); sprintf(s, "%s%s/hark%s", base, before, after);
    return s;
}
int host_exists(const char *path) {
#ifdef _WIN32
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
#else
    struct stat st; if (!lstat(path, &st)) return 1; return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
#endif
}
unsigned long host_pid(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}
int host_mkdirs(const char *path) {
    char *p = xstrdup(path), *q; int ok;
    for (q = p + 1; *q; q++) {
        if (*q != '/'
#ifdef _WIN32
            && *q != '\\'
#endif
        ) continue;
        { char c = *q; *q = 0;
#ifdef _WIN32
          if (!(strlen(p) == 2 && p[1] == ':')) (void)CreateDirectoryA(p, NULL);
#else
          (void)mkdir(p, 0700);
#endif
          *q = c; }
    }
#ifdef _WIN32
    (void)CreateDirectoryA(p, NULL);
    { DWORD attrs = GetFileAttributesA(p); ok = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT); }
#else
    (void)mkdir(p, 0700);
    { struct stat st; ok = !lstat(p, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 022); }
#endif
    free(p); return ok;
}
const char *host_shell(void) {
#ifdef _WIN32
    return "cmd.exe";
#else
    return "/bin/sh";
#endif
}
char *host_cwd(void) { size_t n = 256; char *p;
    for (;;) { p = xmalloc(n);
#ifdef _WIN32
        if (_getcwd(p, (int)n)) return p;
#else
        if (getcwd(p, n)) return p;
#endif
        free(p); if (errno != ERANGE || n >= 1048576) return xstrdup("(unknown)"); n *= 2;
    }
}
int host_chdir(const char *path) {
#ifdef _WIN32
    return _chdir(path);
#else
    return chdir(path);
#endif
}
int host_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin));
#else
    return isatty(STDIN_FILENO);
#endif
}
#ifdef _WIN32
static volatile LONG interrupt_flag; static int interrupt_armed;
static BOOL WINAPI on_console(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    interrupt_flag = 1; return TRUE;
}
void host_interrupt_arm(void) { if (!interrupt_armed && SetConsoleCtrlHandler(on_console, TRUE)) interrupt_armed = 1; }
void host_interrupt_disarm(void) { if (interrupt_armed) (void)SetConsoleCtrlHandler(on_console, FALSE); interrupt_armed = 0; }
#else
static volatile sig_atomic_t interrupt_flag; static int interrupt_armed;
static struct sigaction previous_int;
static void on_sigint(int sig) { (void)sig; interrupt_flag = 1; }
void host_interrupt_arm(void) {
    struct sigaction action;
    if (interrupt_armed) return;
    /* No SA_RESTART, so a blocked terminal read returns EINTR. */
    memset(&action, 0, sizeof action); action.sa_handler = on_sigint; sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_int)) return;
    /* A shell that started Hark with SIGINT ignored (e.g. a background job) keeps it ignored. */
    if (previous_int.sa_handler == SIG_IGN) { (void)sigaction(SIGINT, &previous_int, NULL); return; }
    interrupt_armed = 1;
}
void host_interrupt_disarm(void) { if (interrupt_armed) (void)sigaction(SIGINT, &previous_int, NULL); interrupt_armed = 0; }
#endif
int host_interrupted(void) { return interrupt_flag != 0; }
void host_interrupt_clear(void) { interrupt_flag = 0; }
void host_sleep(unsigned s) {
#ifdef _WIN32
    unsigned long ms = s * 1000UL;
    while (ms && !interrupt_flag) { unsigned long step = ms < 100 ? ms : 100; Sleep(step); ms -= step; }
#else
    struct timespec t; t.tv_sec = (time_t)s; t.tv_nsec = 0; while (nanosleep(&t, &t) && errno == EINTR && !interrupt_flag) {}
#endif
}
static void capture(char *out, size_t *used, const char *s, size_t n, size_t limit, int *truncated) {
    size_t take = n < limit - *used ? n : limit - *used;
    /* Shell output need not be UTF-8. Keep bytes, replace NULs for JSON strings. */
    size_t i; for (i = 0; i < take; i++) out[*used + i] = s[i] ? s[i] : '?';
    *used += take; out[*used] = 0; if (take < n) *truncated = 1;
}
#ifndef _WIN32
static volatile sig_atomic_t terminated;
static void on_terminate(int sig) { terminated = sig; }
#ifdef CLOCK_MONOTONIC
static double now(void) { struct timespec t; if (clock_gettime(CLOCK_MONOTONIC, &t)) return (double)time(NULL); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
#else
/* Mac OS X before 10.12 has no clock_gettime; wall time is the best available. */
static double now(void) { struct timeval t; if (gettimeofday(&t, NULL)) return (double)time(NULL); return (double)t.tv_sec + (double)t.tv_usec / 1e6; }
#endif
char *host_run(const char *shell, const char *command, unsigned seconds, size_t limit, int *status, int *timed, int *truncated) {
    int fd[2], st = 0, done = 0, eof = 0, stopped = 0; pid_t pid; size_t used = 0; double deadline;
    struct sigaction action, old_term;
    char block[4096], *out = xmalloc(limit + 1); out[0] = 0; *status = -1; *timed = *truncated = 0;
    if (pipe(fd)) { free(out); return xstrdup("cannot create shell pipe"); }
    fflush(NULL); pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); free(out); return xstrdup("cannot fork shell"); }
    if (!pid) {
        int nullfd; close(fd[0]); if (setpgid(0, 0)) _exit(126);
        nullfd = open("/dev/null", O_RDONLY); if (nullfd < 0) _exit(126);
        if (dup2(nullfd, 0) < 0 || dup2(fd[1], 1) < 0 || dup2(fd[1], 2) < 0) _exit(126);
        if (nullfd > 2) close(nullfd);
        if (fd[1] > 2) close(fd[1]);
        execlp(shell, shell, "-c", command, (char *)NULL); _exit(127);
    }
    close(fd[1]); (void)setpgid(pid, pid);
    if (fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) { (void)kill(-pid, SIGKILL); close(fd[0]); while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {} free(out); return xstrdup("cannot configure shell pipe"); }
    /* SIGINT is the caller's (host_interrupt_arm); the shell's own group never sees ^C. */
    memset(&action, 0, sizeof action); action.sa_handler = on_terminate; sigemptyset(&action.sa_mask);
    terminated = 0; (void)sigaction(SIGTERM, &action, &old_term);
    deadline = now() + seconds;
    while (!done || !eof) {
        struct pollfd p; ssize_t n; pid_t r;
        /* Bound draining as well: an endless writer must not starve the timeout. */
        int rounds = 0;
        while (rounds++ < 16 && (n = read(fd[0], block, sizeof block)) > 0) capture(out, &used, block, (size_t)n, limit, truncated);
        if (rounds <= 16 && n == 0) eof = 1;
        if (!done) { r = waitpid(pid, &st, WNOHANG); if (r == pid || (r < 0 && errno != EINTR)) done = 1; }
        if (done && eof) break;
        if (terminated || interrupt_flag || now() >= deadline) {
            stopped = terminated || interrupt_flag; *timed = !stopped;
            (void)kill(-pid, SIGKILL); if (!done) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            break;
        }
        p.fd = fd[0]; p.events = POLLIN; p.revents = 0; (void)poll(&p, 1, eof ? 20 : 50);
    }
    /* Do not leave background jobs running when their shell exits. */
    (void)kill(-pid, SIGKILL); close(fd[0]);
    (void)sigaction(SIGTERM, &old_term, NULL);
    if (terminated) { int sig = terminated; free(out); raise(sig); exit(128 + sig); }
    *status = *timed ? 124 : stopped ? 130 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1;
    return out;
}
char *host_read_private(const char *path, size_t limit) {
    struct stat st; int fd; FILE *f; char *s; size_t n;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return NULL;
    fd = open(path, O_RDONLY
#ifdef O_NOFOLLOW
              | O_NOFOLLOW
#endif
    );
    if (fd < 0) return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077) || st.st_size < 0 || (unsigned long long)st.st_size > (unsigned long long)limit) { close(fd); return NULL; }
    f = fdopen(fd, "rb"); if (!f) { close(fd); return NULL; }
    s = xmalloc((size_t)st.st_size + 1); n = fread(s, 1, (size_t)st.st_size, f); s[n] = 0;
    if (ferror(f) || n != (size_t)st.st_size || memchr(s, 0, n)) { free(s); s = NULL; } fclose(f); return s;
}
static int write_private(const char *path, const char *s, int replace) {
    size_t n = strlen(path) + 16; char *tmp = xmalloc(n); int fd, ok = 0; FILE *f;
    snprintf(tmp, n, "%s.XXXXXX", path); fd = mkstemp(tmp);
    if (fd < 0) { free(tmp); return 0; }
    f = fdopen(fd, "wb");
    if (f) { int written = fputs(s, f) >= 0 && !fflush(f) && !fsync(fd); int closed = fclose(f) == 0; if (written && closed) { if (replace) ok = rename(tmp, path) == 0; else if (link(tmp, path) == 0) { ok = 1; unlink(tmp); } } }
    else close(fd);
    if (!ok) unlink(tmp);
    free(tmp); return ok;
}
#else
/* Quote one argv element according to the Windows C runtime rules. */
static char *winquote(const char *s) {
    size_t n = strlen(s), i, k = 0, slash = 0; char *q = xmalloc(n * 2 + 3); q[k++] = '"';
    for (i = 0; i < n; i++) {
        if (s[i] == '\\') { slash++; continue; }
        if (s[i] == '"') { while (slash) { q[k++] = '\\'; q[k++] = '\\'; slash--; } q[k++] = '\\'; }
        else while (slash) { q[k++] = '\\'; slash--; }
        q[k++] = s[i];
    }
    while (slash) { q[k++] = '\\'; q[k++] = '\\'; slash--; } q[k++] = '"'; q[k] = 0; return q;
}
char *host_run(const char *shell, const char *command, unsigned seconds, size_t limit, int *status, int *timed, int *truncated) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE}; STARTUPINFOA si; PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ji; HANDLE rd = NULL, wr = NULL, input = INVALID_HANDLE_VALUE, job = NULL;
    char *a = winquote(shell), *b = NULL, *line = NULL, block[4096], *out = xmalloc(limit + 1); size_t used = 0;
    const char *base = strrchr(shell, '\\'); DWORD got, available, code, start = GetTickCount(); int cmd, stopped = 0;
    if (!base) base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    cmd = !_stricmp(base, "cmd.exe") || !_stricmp(base, "cmd");
    /* cmd's /s /c outer quotes are not C-runtime argv quoting. */
    if (cmd) { b = xmalloc(strlen(command) + 3); sprintf(b, "\"%s\"", command); } else b = winquote(command);
    line = xmalloc(strlen(a) + strlen(b) + 20); sprintf(line, "%s %s %s", a, cmd ? "/d /s /c" : "-c", b);
    free(a); free(b); out[0] = 0; *status = -1; *timed = *truncated = 0;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi); si.cb = sizeof si;
    if (!CreatePipe(&rd, &wr, &sa, 0) || !SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0)) goto fail;
    input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (input == INVALID_HANDLE_VALUE) goto fail;
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = input; si.hStdOutput = si.hStdError = wr;
    job = CreateJobObjectA(NULL, NULL); memset(&ji, 0, sizeof ji); ji.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &ji, sizeof ji)) goto fail;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) goto fail;
    if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 126); goto fail; }
    ResumeThread(pi.hThread); CloseHandle(wr); wr = NULL;
    for (;;) {
        int rounds = 0;
        while (rounds++ < 16 && PeekNamedPipe(rd, NULL, 0, NULL, &available, NULL) && available) {
            if (!ReadFile(rd, block, available < sizeof block ? available : (DWORD)sizeof block, &got, NULL)) break;
            capture(out, &used, block, got, limit, truncated);
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &available, NULL) || !available) break;
        }
        if (interrupt_flag) { stopped = 1; TerminateJobObject(job, 130); break; }
        if ((DWORD)(GetTickCount() - start) >= seconds * 1000UL) { *timed = 1; TerminateJobObject(job, 124); break; }
        Sleep(20);
    }
    GetExitCodeProcess(pi.hProcess, &code); *status = *timed ? 124 : stopped ? 130 : (int)code;
    goto cleanup;
fail:
    free(out); out = xstrdup("cannot start shell (process/pipe/job setup failed)");
cleanup:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (rd) CloseHandle(rd);
    if (wr) CloseHandle(wr);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    free(line); return out;
}
char *host_read_private(const char *path, size_t limit) {
    HANDLE f; DWORD size, n; char *s;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    { BY_HANDLE_FILE_INFORMATION info; if (!GetFileInformationByHandle(f, &info) || info.nFileSizeHigh || (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) { CloseHandle(f); return NULL; } }
    size = GetFileSize(f, NULL); if (size > limit) { CloseHandle(f); return NULL; }
    s = xmalloc((size_t)size + 1); if (!ReadFile(f, s, size, &n, NULL) || n != size) { free(s); CloseHandle(f); return NULL; }
    if (memchr(s, 0, n)) { free(s); CloseHandle(f); return NULL; }
    s[n] = 0; CloseHandle(f); return s;
}
static int write_private(const char *path, const char *s, int replace) {
    PSECURITY_DESCRIPTOR sd = NULL; SECURITY_ATTRIBUTES sa; HANDLE f; DWORD n; int ok = 0, tries;
    char *tmp = xmalloc(strlen(path) + 64);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA("D:P(A;;GA;;;OW)", SDDL_REVISION_1, &sd, NULL)) { free(tmp); return 0; }
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = sd; sa.bInheritHandle = FALSE;
    f = INVALID_HANDLE_VALUE;
    for (tries = 0; tries < 100; tries++) { sprintf(tmp, "%s.%lu.%lu.%d.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), tries); f = CreateFileA(tmp, GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL); if (f != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break; }
    if (f != INVALID_HANDLE_VALUE) {
        ok = WriteFile(f, s, (DWORD)strlen(s), &n, NULL) && n == strlen(s) && FlushFileBuffers(f);
        CloseHandle(f); if (ok) ok = MoveFileExA(tmp, path, (replace ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileA(tmp);
    }
    LocalFree(sd); free(tmp); return ok;
}
#endif

char *host_read_secret(const char *path) { return host_read_private(path, 1048576); }

int host_write_secret(const char *path, const char *s) { return write_private(path, s, 1); }
int host_write_new_private(const char *path, const char *s) { return write_private(path, s, 0); }

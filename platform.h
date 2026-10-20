/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_PLATFORM_H
#define HARK_PLATFORM_H
#include <stddef.h>
/* Host port boundary: no ISA assumptions. WASI needs implementations of these. */
const char *host_shell(void);
const char *host_home(void);
/* Per-user Hark directories, following the XDG base directory spec:
 * $XDG_CONFIG_HOME/hark, $XDG_DATA_HOME/hark, $XDG_STATE_HOME/hark and
 * $XDG_CACHE_HOME/hark. Unset or relative variables fall back to ~/.config,
 * ~/.local/share, ~/.local/state and ~/.cache (on Windows, %APPDATA%\hark and
 * %LOCALAPPDATA%\hark, with state and cache in subdirectories). NULL without a
 * home directory. The directory is not created; see host_mkdirs. */
enum { HOST_CONFIG, HOST_DATA, HOST_STATE, HOST_CACHE };
char *host_dir(int kind);
/* Create a directory and missing parents. New directories are owner-only, and
 * the final one must be a real directory owned by the user, writable only by them. */
int host_mkdirs(const char *path);
int host_exists(const char *path); /* 1 present (even a dangling link), 0 absent, -1 unknown */
unsigned long host_pid(void);
char *host_cwd(void);
int host_chdir(const char *path);
int host_tty(void);
int host_stdout_tty(void);
void host_enable_ansi(void); /* Windows consoles need this for ANSI escapes. */
void host_sleep(unsigned seconds);
double host_seconds(void); /* Monotonic where available; for measuring intervals. */
/* While armed, ^C sets a flag instead of terminating Hark: requests, shell
 * commands and sleeps stop early, and blocking terminal reads return. */
void host_interrupt_arm(void);
void host_interrupt_disarm(void);
int host_interrupted(void);
void host_interrupt_clear(void);
void host_interrupt_hook(void (*hook)(void));
char *host_run(const char *shell, const char *command, unsigned seconds,
               size_t limit, int *status, int *timed_out, int *truncated);
/* Background jobs: like host_run, but asynchronous. A job keeps running, with its
 * output captured and timeout enforced, until it exits, times out or is killed.
 * host_job_wait waits up to MS milliseconds (returning early on ^C) and returns 1
 * once the job has finished; host_job_result then gives its output and status. */
typedef struct HostJob HostJob;
HostJob *host_job_start(const char *shell, const char *command, unsigned seconds, size_t limit);
int host_job_wait(HostJob *job, unsigned ms);
const char *host_job_result(const HostJob *job, int *status, int *timed_out, int *truncated, int *killed);
void host_job_kill(HostJob *job);
void host_job_free(HostJob *job); /* Kills the job if it is still running. */
char *host_read_private(const char *path, size_t limit);
char *host_read_secret(const char *path); /* NULL on missing/unsafe file */
int host_write_secret(const char *path, const char *text);
int host_write_new_private(const char *path, const char *text);
/* Print untrusted data without terminal control sequences. */
void host_print(const char *text);
void host_eprint(const char *text); /* The same, to stderr. */
#endif

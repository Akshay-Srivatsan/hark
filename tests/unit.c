/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "json.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif
static void json_tests(void) {
    const char *bad[] = {"", "[", "{", "null trailing", "01", "1.", "1e", "[1,]", "{\"x\":1,\"x\":2}", "\"\\u0000\"", "\"\\ud800\"", "\"\xc0\x80\"", "NaN", "{\"x\":\"a\\u0000b\"}", NULL};
    int i; Json *j, *copy; char *s; char deep[400];
    for (i = 0; bad[i]; i++) assert(!jparse(bad[i]));
    j = jparse("{\"n\":18446744073709551615,\"s\":\"\\ud83d\\ude80\\n\\\\\\\"\",\"a\":[true,false,null,-1.25e4]}"); assert(j);
    s = jdump(j); copy = jparse(s); assert(copy); assert(!strcmp(jtext(jget(j, "s")), jtext(jget(copy, "s"))));
    assert(strstr(s, "18446744073709551615")); free(s); jfree(copy); jfree(j);
    memset(deep, '[', 150); memset(deep + 150, ']', 150); deep[300] = 0; assert(!jparse(deep));
    j = jstr("bad\xff"); s = jdump(j); copy = jparse(s); assert(copy); assert(strstr(s, "\\uFFFD") || strstr(s, "\\ufffd")); free(s); jfree(copy); jfree(j);
}
static void shell_tests(void) {
    char *s; int st, timed, trunc;
    s = host_run(host_shell(), "printf 'one'; printf 'two' >&2; exit 7", 5, 100, &st, &timed, &trunc);
    assert(st == 7 && !timed && !trunc && !strcmp(s, "onetwo")); free(s);
    s = host_run(host_shell(), "printf 'abcdef'", 5, 3, &st, &timed, &trunc);
    assert(st == 0 && trunc && !strcmp(s, "abc")); free(s);
    s = host_run(host_shell(), "read line; printf 'done'", 5, 100, &st, &timed, &trunc);
    assert(st == 0 && !strcmp(s, "done")); free(s);
    s = host_run(host_shell(), "sleep 5", 1, 100, &st, &timed, &trunc);
    assert(st == 124 && timed); free(s);
    s = host_run(host_shell(), "while :; do printf 'data'; done", 1, 100, &st, &timed, &trunc);
    assert(st == 124 && timed && trunc); free(s);
    s = host_run("/nonexistent/hark-shell", "true", 1, 100, &st, &timed, &trunc);
    assert(st == 127); free(s);
}
#ifndef _WIN32
static void secret_tests(void) {
    char path[] = "/tmp/hark-unit-XXXXXX", linkpath[128]; int fd = mkstemp(path); char *s; struct stat st;
    assert(fd >= 0); close(fd); assert(host_write_secret(path, "secret"));
    assert(!host_write_new_private(path, "must not overwrite"));
    assert(!stat(path, &st) && (st.st_mode & 0777) == 0600);
    s = host_read_secret(path); assert(s && !strcmp(s, "secret")); free(s);
    snprintf(linkpath, sizeof linkpath, "%s-link", path); assert(!symlink(path, linkpath)); assert(!host_read_secret(linkpath)); unlink(linkpath);
    assert(!chmod(path, 0644)); assert(!host_read_secret(path)); unlink(path);
}
#endif
int main(void) { json_tests(); shell_tests();
#ifndef _WIN32
    secret_tests();
#endif
    puts("unit tests passed"); return 0;
}

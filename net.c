/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net.h"
#include "platform.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HTTP_LIMIT (32UL * 1024UL * 1024UL)
typedef struct { char *s; size_t n; } Body;
static size_t receive(char *s, size_t size, size_t count, void *ctx) {
    Body *b = ctx; size_t n;
    if (size && count > (size_t)-1 / size) return 0;
    n = size * count; if (host_interrupted() || n > HTTP_LIMIT - b->n || memchr(s, 0, n)) return 0;
    b->s = xrealloc(b->s, b->n + n + 1); memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0; return n;
}
static struct curl_slist *header(struct curl_slist *h, const char *name, const char *value) {
    char *s; struct curl_slist *next;
    if (strpbrk(value, "\r\n")) { fputs("invalid HTTP header\n", stderr); exit(1); }
    s = xmalloc(strlen(name) + strlen(value) + 3); sprintf(s, "%s: %s", name, value);
    next = curl_slist_append(h, s); free(s); if (!next) { fputs("out of memory\n", stderr); exit(1); } return next;
}
/* libcurl polls this about once a second while idle; nonzero aborts on ^C. */
#if LIBCURL_VERSION_NUM >= 0x072000
static int progress(void *ctx, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
#else
static int progress(void *ctx, double a, double b, double c, double d) {
#endif
    (void)ctx; (void)a; (void)b; (void)c; (void)d; return host_interrupted();
}
static int has_scheme(const char *url, const char *scheme) {
    for (; *scheme; url++, scheme++) if (tolower((unsigned char)*url) != *scheme) return 0;
    return 1;
}
static Http http_request(const char *url, const char *key, const char *account, const char *type, const char *body, unsigned timeout) {
    Http r = {NULL, 0}; Body b = {NULL, 0}; CURL *c = curl_easy_init(); CURLcode rc; struct curl_slist *h = NULL; char err[CURL_ERROR_SIZE] = {0};
    /* Older libcurl lacks CURLOPT_PROTOCOLS, so check the scheme here too. */
    if (!has_scheme(url, "http://") && !has_scheme(url, "https://")) { fputs("only HTTP and HTTPS URLs are supported\n", stderr); if (c) curl_easy_cleanup(c); return r; }
    if (!c) { fputs("cannot initialize HTTP\n", stderr); return r; }
    if (host_interrupted()) { curl_easy_cleanup(c); return r; }
    if (type) h = header(h, "Content-Type", type);
    h = header(h, "Accept", body ? "application/json, text/event-stream" : "application/json");
    if (key && *key) { char *v = xmalloc(strlen(key) + 8); sprintf(v, "Bearer %s", key); h = header(h, "Authorization", v); free(v); }
    if (account && *account) h = header(h, "ChatGPT-Account-Id", account);
    h = header(h, "originator", "hark");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    } else curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, err);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
#else
    curl_easy_setopt(c, CURLOPT_PROGRESSFUNCTION, progress);
#endif
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
#elif LIBCURL_VERSION_NUM >= 0x071304
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(c, CURLOPT_USERAGENT, "hark/0.1");
    rc = curl_easy_perform(c);
    if (rc == CURLE_OK) { curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status); r.body = b.s ? b.s : xstrdup(""); }
    else { if (!host_interrupted()) fprintf(stderr, "HTTP transport failed: %s\n", err[0] ? err : curl_easy_strerror(rc)); free(b.s); }
    curl_slist_free_all(h); curl_easy_cleanup(c); return r;
}
Http http_post(const char *url, const char *key, const char *account, const char *type, const char *body, unsigned timeout) {
    return http_request(url, key, account, type, body, timeout);
}
Http http_get(const char *url, const char *key, const char *account, unsigned timeout) {
    return http_request(url, key, account, NULL, NULL, timeout);
}
void http_free(Http *r) { free(r->body); r->body = NULL; }
int http_ok(const Http *r) { return r->body && r->status >= 200 && r->status < 300; }
#define ERROR_LIMIT 400
/* Only the provider's error message is shown, never the rest of the body. */
static char *error_message(const Http *r, const char *secret) {
    Json *j = r->body ? jparse(r->body) : NULL, *e = jget(j, "error"), *m = NULL; char *s = NULL, *hit; size_t n;
    if (e && e->type == J_OBJ) m = jget(e, "message");
    else if (e && e->type == J_STR) m = e;
    if (!m || m->type != J_STR) m = jget(j, "message");
    if (!m || m->type != J_STR) m = jget(j, "detail");
    if (m && m->type == J_STR && *m->s) {
        s = xstrdup(m->s);
        /* Servers sometimes echo the credential back; never print it. */
        if (secret && strlen(secret) >= 8) while ((hit = strstr(s, secret)) != NULL) {
            char *next = xmalloc(strlen(s) - strlen(secret) + 11);
            memcpy(next, s, (size_t)(hit - s)); strcpy(next + (hit - s), "[redacted]"); strcat(next, hit + strlen(secret));
            free(s); s = next;
        }
        if ((n = strlen(s)) > ERROR_LIMIT) {
            n = ERROR_LIMIT; while (n && ((unsigned char)s[n] & 0xc0) == 0x80) n--; /* Keep whole UTF-8 characters. */
            strcpy(s + n, "...");
        }
    }
    jfree(j); return s;
}
void http_report(const char *what, const Http *r, const char *note, const char *secret) {
    char *message;
    if (host_interrupted()) return;
    fprintf(stderr, "%s (HTTP %ld%s%s)", what, r->status, note ? ", " : "", note ? note : "");
    if ((message = error_message(r, secret)) != NULL) { fputs(": ", stderr); host_eprint(message); free(message); }
    fputc('\n', stderr);
}
Json *http_json(const Http *r, const char *secret) {
    Json *j = r->body ? jparse(r->body) : NULL;
    if (!http_ok(r) || !j || j->type != J_OBJ || (jget(j, "error") && jget(j, "error")->type != J_NULL)) {
        http_report("provider request failed", r, NULL, secret);
        jfree(j); return NULL;
    }
    return j;
}
/* A completed item can carry text, reasoning, or a function call. Codex's
 * final response often has metadata only, so never discard item.done events. */
static int stream_event(const char *data, Json *items, Json **result, size_t *count) {
    Json *event, *index, *item; char *end; unsigned long n;
    if (!*data || !strcmp(data, "[DONE]")) return 1;
    event = jparse(data); if (!event || event->type != J_OBJ) { jfree(event); return 0; }
    if (jis(event, "type", "error") || jis(event, "type", "response.failed") || jis(event, "type", "response.incomplete")) { jfree(event); return 0; }
    if (jis(event, "type", "response.output_item.done")) {
        index = jget(event, "output_index"); item = jget(event, "item");
        if (!index || index->type != J_NUM || index->s[0] < '0' || index->s[0] > '9' || !item || item->type != J_OBJ) { jfree(event); return 0; }
        n = strtoul(index->s, &end, 10);
        if (*end || n >= 1024) { jfree(event); return 0; }
        jset(items, index->s, jcopy(item)); if (n + 1 > *count) *count = (size_t)n + 1;
    } else if (jis(event, "type", "response.completed")) {
        Json *response = jget(event, "response");
        if (!response || response->type != J_OBJ || *result) { jfree(event); return 0; }
        *result = jcopy(response);
        if (!jget(*result, "status")) jset(*result, "status", jstr("completed"));
    }
    jfree(event); return 1;
}
Json *response_json(const char *s) {
    Json *result = jparse(s), *items, *out, *v; size_t count = 0, used = 0, i;
    char *data;
    if (result) return result;
    items = jnew(J_OBJ); data = xstrdup("");
    /* Parse SSE frames, including CRLF and multi-line data fields. Ignore
     * comments/other fields; never accept a stream without completion. */
    while (*s) {
        const char *end = strchr(s, '\n'); size_t n = end ? (size_t)(end - s) : strlen(s);
        if (n && s[n - 1] == '\r') n--;
        if (!n) {
            if (!stream_event(data, items, &result, &count)) goto bad;
            used = 0; data[0] = 0;
        } else if (n >= 5 && !strncmp(s, "data:", 5)) {
            size_t skip = 5; if (n > skip && s[skip] == ' ') skip++;
            data = xrealloc(data, used + n - skip + 2);
            if (used) data[used++] = '\n';
            memcpy(data + used, s + skip, n - skip); used += n - skip; data[used] = 0;
        }
        if (!end) break;
        s = end + 1;
    }
    if (used && !stream_event(data, items, &result, &count)) goto bad;
    if (!result) goto bad;
    if (count) {
        Json *final = jget(result, "output"), *f = final && final->type == J_ARR ? final->child : NULL;
        out = jnew(J_ARR);
        for (i = 0; i < count || f; i++) {
            char key[32]; snprintf(key, sizeof key, "%lu", (unsigned long)i); v = jget(items, key);
            if (!v) v = f;
            if (!v) { jfree(out); goto bad; }
            jadd(out, jcopy(v)); if (f) f = f->next;
        }
        jset(result, "output", out);
    } else if (!jget(result, "output")) jset(result, "output", jnew(J_ARR));
    free(data); jfree(items); return result;
bad:
    free(data); jfree(items); jfree(result); return NULL;
}

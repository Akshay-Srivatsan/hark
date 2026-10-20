/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net.h"
#include "platform.h"
#include <curl/curl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HTTP_LIMIT (32UL * 1024UL * 1024UL)
/* Incremental server-sent events: "data:" lines (CRLF or LF, possibly several per
 * event) are joined and passed to FN at each blank line. Comments and other
 * fields are ignored. One parser serves live display and final assembly. */
typedef struct { char *line, *data; size_t lused, dused; int (*fn)(const char *data, void *ctx); void *ctx; int failed; } Sse;
static void sse_dispatch(Sse *e) {
    if (e->dused && !e->failed && !e->fn(e->data, e->ctx)) e->failed = 1;
    e->dused = 0;
}
static void sse_line(Sse *e) {
    size_t n = e->lused, skip = 5;
    if (n && e->line[n - 1] == '\r') n--;
    if (!n) sse_dispatch(e);
    else if (n >= 5 && !strncmp(e->line, "data:", 5)) {
        if (n > skip && e->line[skip] == ' ') skip++;
        e->data = xrealloc(e->data, e->dused + n - skip + 2);
        if (e->dused) e->data[e->dused++] = '\n';
        memcpy(e->data + e->dused, e->line + skip, n - skip); e->dused += n - skip; e->data[e->dused] = 0;
    }
    e->lused = 0;
}
static void sse_feed(Sse *e, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n') { sse_line(e); continue; }
        if (!(e->lused & 1023)) e->line = xrealloc(e->line, e->lused + 1025);
        e->line[e->lused++] = s[i];
    }
}
static int sse_finish(Sse *e) {
    if (e->lused) sse_line(e);
    sse_dispatch(e); free(e->line); free(e->data); e->line = e->data = NULL; return !e->failed;
}
typedef struct { char *s; size_t n; Sse *live; } Body;
static size_t receive(char *s, size_t size, size_t count, void *ctx) {
    Body *b = ctx; size_t n;
    if (size && count > (size_t)-1 / size) return 0;
    n = size * count; if (host_interrupted() || n > HTTP_LIMIT - b->n || memchr(s, 0, n)) return 0;
    b->s = xrealloc(b->s, b->n + n + 1); memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
    if (b->live) sse_feed(b->live, s, n);
    return n;
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
static Http http_request(const char *url, const char *key, const char *account, const char *type, const char *body, unsigned timeout, Sse *live) {
    Http r = {NULL, 0}; Body b = {NULL, 0, NULL}; CURL *c = curl_easy_init(); CURLcode rc; struct curl_slist *h = NULL; char err[CURL_ERROR_SIZE] = {0};
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
    b.live = live; curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
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
    if (live) (void)sse_finish(live); /* Display only; the body is assembled separately. */
    if (rc == CURLE_OK) { curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status); r.body = b.s ? b.s : xstrdup(""); }
    else { if (!host_interrupted()) fprintf(stderr, "HTTP transport failed: %s\n", err[0] ? err : curl_easy_strerror(rc)); free(b.s); }
    curl_slist_free_all(h); curl_easy_cleanup(c); return r;
}
Http http_post(const char *url, const char *key, const char *account, const char *type, const char *body, unsigned timeout) {
    return http_request(url, key, account, type, body, timeout, NULL);
}
Http http_post_stream(const char *url, const char *key, const char *account, const char *type, const char *body, unsigned timeout, int (*on_event)(const char *data, void *ctx), void *ctx) {
    Sse live; memset(&live, 0, sizeof live); live.fn = on_event; live.ctx = ctx;
    return http_request(url, key, account, type, body, timeout, on_event ? &live : NULL);
}
Http http_get(const char *url, const char *key, const char *account, unsigned timeout) {
    return http_request(url, key, account, NULL, NULL, timeout, NULL);
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
typedef struct { Json *items, *result; size_t count; } ResponsesStream;
static int stream_event(const char *data, void *ctx) {
    ResponsesStream *rs = ctx; Json *items = rs->items, **result = &rs->result; size_t *count = &rs->count;
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
    Json *result = jparse(s), *out, *v; ResponsesStream rs; Sse e; size_t i;
    if (result) return result;
    /* Never accept a stream without a successful completion event. */
    memset(&rs, 0, sizeof rs); rs.items = jnew(J_OBJ);
    memset(&e, 0, sizeof e); e.fn = stream_event; e.ctx = &rs;
    sse_feed(&e, s, strlen(s));
    if (!sse_finish(&e) || !rs.result) goto bad;
    result = rs.result; rs.result = NULL;
    if (rs.count) {
        Json *final = jget(result, "output"), *f = final && final->type == J_ARR ? final->child : NULL;
        out = jnew(J_ARR);
        for (i = 0; i < rs.count || f; i++) {
            char key[32]; snprintf(key, sizeof key, "%lu", (unsigned long)i); v = jget(rs.items, key);
            if (!v) v = f;
            if (!v) { jfree(out); goto bad; }
            jadd(out, jcopy(v)); if (f) f = f->next;
        }
        jset(result, "output", out);
    } else if (!jget(result, "output")) jset(result, "output", jnew(J_ARR));
    jfree(rs.items); return result;
bad:
    jfree(rs.items); jfree(rs.result); jfree(result); return NULL; /* At most one is set. */
}
/* Chat Completions streams send choices[0].delta fragments: text, reasoning, and
 * tool calls split by index (id and name first, arguments in pieces). They are
 * rebuilt into the nonstreaming shape; without a finish_reason the response
 * stays incomplete, so no tool runs. */
#define CHAT_CALLS 64
typedef struct { char *content, *reasoning, *finish; size_t ncontent, nreasoning; Json *calls[CHAT_CALLS], *usage; size_t ncalls; } ChatStream;
static void append(char **s, size_t *n, const char *part) {
    size_t k = strlen(part); *s = xrealloc(*s, *n + k + 1); memcpy(*s + *n, part, k + 1); *n += k;
}
static void append_field(Json *obj, const char *key, const char *part) {
    Json *v = jget(obj, key); size_t n = strlen(v->s); append(&v->s, &n, part);
}
static int chat_event(const char *data, void *ctx) {
    ChatStream *c = ctx; Json *chunk, *choices, *choice, *delta, *v, *t;
    if (!*data || !strcmp(data, "[DONE]")) return 1;
    chunk = jparse(data);
    if (!chunk || chunk->type != J_OBJ || (jget(chunk, "error") && jget(chunk, "error")->type != J_NULL)) { jfree(chunk); return 0; }
    /* Usage usually arrives in a final chunk with no choices. */
    if ((v = jget(chunk, "usage")) && v->type == J_OBJ) { jfree(c->usage); c->usage = jcopy(v); }
    choices = jget(chunk, "choices"); choice = choices && choices->type == J_ARR ? choices->child : NULL;
    delta = jget(choice, "delta");
    if ((v = jget(delta, "content")) && v->type == J_STR) append(&c->content, &c->ncontent, v->s);
    if (((v = jget(delta, "reasoning_content")) || (v = jget(delta, "reasoning"))) && v->type == J_STR) append(&c->reasoning, &c->nreasoning, v->s);
    for (t = (v = jget(delta, "tool_calls")) && v->type == J_ARR ? v->child : NULL; t; t = t->next) {
        Json *index = jget(t, "index"), *f = jget(t, "function"), *slot, *part; char *end; unsigned long k = c->ncalls ? c->ncalls - 1 : 0;
        if (index && index->type == J_NUM) { k = strtoul(index->s, &end, 10); if (*end) { jfree(chunk); return 0; } }
        if (k >= CHAT_CALLS || k > c->ncalls) { jfree(chunk); return 0; } /* Indexes arrive in order. */
        if (k == c->ncalls) {
            slot = jnew(J_OBJ); jset(slot, "id", jstr("")); jset(slot, "type", jstr("function"));
            part = jnew(J_OBJ); jset(part, "name", jstr("")); jset(part, "arguments", jstr("")); jset(slot, "function", part);
            c->calls[c->ncalls++] = slot;
        }
        slot = c->calls[k]; part = jget(slot, "function");
        if ((v = jget(t, "id")) && v->type == J_STR && !*jget(slot, "id")->s) append_field(slot, "id", v->s);
        /* Some servers repeat the whole name in every fragment. */
        if ((v = jget(f, "name")) && v->type == J_STR && strcmp(jget(part, "name")->s, v->s)) append_field(part, "name", v->s);
        if ((v = jget(f, "arguments")) && v->type == J_STR) append_field(part, "arguments", v->s);
    }
    if ((v = jget(choice, "finish_reason")) && v->type == J_STR) { free(c->finish); c->finish = xstrdup(v->s); }
    jfree(chunk); return 1;
}
Json *chat_response_json(const char *s) {
    Json *result = jparse(s), *choices, *choice, *msg, *calls; ChatStream c; Sse e; size_t i;
    if (result) return result;
    memset(&c, 0, sizeof c); memset(&e, 0, sizeof e); e.fn = chat_event; e.ctx = &c;
    sse_feed(&e, s, strlen(s));
    if (!sse_finish(&e) || (!c.ncontent && !c.ncalls && !c.finish)) {
        for (i = 0; i < c.ncalls; i++) jfree(c.calls[i]);
        jfree(c.usage); free(c.content); free(c.reasoning); free(c.finish); return NULL;
    }
    msg = jnew(J_OBJ); jset(msg, "role", jstr("assistant"));
    jset(msg, "content", c.ncontent || !c.ncalls ? jstr(c.content ? c.content : "") : jnew(J_NULL));
    if (c.nreasoning) jset(msg, "reasoning_content", jstr(c.reasoning));
    if (c.ncalls) { calls = jnew(J_ARR); for (i = 0; i < c.ncalls; i++) jadd(calls, c.calls[i]); jset(msg, "tool_calls", calls); }
    choice = jnew(J_OBJ); jset(choice, "index", jnum(0)); jset(choice, "message", msg);
    jset(choice, "finish_reason", c.finish ? jstr(c.finish) : jnew(J_NULL));
    choices = jnew(J_ARR); jadd(choices, choice);
    result = jnew(J_OBJ); jset(result, "choices", choices); if (c.usage) jset(result, "usage", c.usage);
    free(c.content); free(c.reasoning); free(c.finish); return result;
}

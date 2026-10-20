/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"
#include "vendor/yyjson/yyjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("out of memory\n", stderr); exit(1); } return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { fputs("out of memory\n", stderr); exit(1); } return p; }
char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *p = xmalloc(n); memcpy(p, s, n); return p; }
Json *jnew(int t) { Json *j = xmalloc(sizeof *j); memset(j, 0, sizeof *j); j->type = t; return j; }
Json *jstr(const char *s) { Json *j = jnew(J_STR); j->s = xstrdup(s); return j; }
Json *jnum(unsigned long n) { char s[32]; Json *j = jnew(J_NUM); sprintf(s, "%lu", n); j->s = xstrdup(s); return j; }
Json *jbool(int b) { Json *j = jnew(J_BOOL); j->s = xstrdup(b ? "true" : "false"); return j; }
void jadd(Json *p, Json *v) { Json **q = &p->child; while (*q) q = &(*q)->next; *q = v; }
void jset(Json *p, const char *k, Json *v) { Json **q = &p->child; while (*q && strcmp((*q)->key, k)) q = &(*q)->next; v->key = xstrdup(k); if (*q) { Json *old = *q; v->next = old->next; old->next = NULL; jfree(old); } *q = v; }
Json *jget(const Json *p, const char *k) { Json *q; if (!p || p->type != J_OBJ) return NULL; for (q = p->child; q; q = q->next) if (!strcmp(q->key, k)) return q; return NULL; }
const char *jtext(const Json *j) { return j && j->type == J_STR ? j->s : ""; }
int jis(const Json *p, const char *k, const char *v) { Json *j = jget(p, k); return j && j->type == J_STR && !strcmp(j->s, v); }
void jfree(Json *j) { while (j) { Json *n = j->next; jfree(j->child); free(j->s); free(j->key); free(j); j = n; } }

/* yyjson performs all parsing, Unicode validation and JSON escaping. This
 * adapter keeps ownership independent of a provider document's lifetime. */
static Json *from_value(yyjson_val *v, unsigned depth) {
    Json *j = NULL, *item; size_t i, count; yyjson_val *key, *value;
    if (depth > 128) return NULL;
    if (yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        if (strlen(s) != yyjson_get_len(v)) return NULL; /* reject embedded NUL */
        return jstr(s);
    }
    if (yyjson_is_raw(v)) {
        const char *raw = yyjson_get_raw(v); size_t n = yyjson_get_len(v);
        j = jnew(J_NUM); j->s = xmalloc(n + 1); memcpy(j->s, raw, n); j->s[n] = 0;
        return j;
    }
    if (yyjson_is_null(v)) return jnew(J_NULL);
    if (yyjson_is_bool(v)) return jbool(yyjson_get_bool(v));
    if (yyjson_is_arr(v)) {
        j = jnew(J_ARR);
        yyjson_arr_foreach(v, i, count, value) {
            item = from_value(value, depth + 1); if (!item) goto bad;
            jadd(j, item);
        }
    } else if (yyjson_is_obj(v)) {
        j = jnew(J_OBJ);
        yyjson_obj_foreach(v, i, count, key, value) {
            const char *name = yyjson_get_str(key);
            if (strlen(name) != yyjson_get_len(key) || jget(j, name)) goto bad;
            item = from_value(value, depth + 1); if (!item) goto bad;
            jset(j, name, item);
        }
    }
    return j;
bad: jfree(j); return NULL;
}
Json *jparse(const char *s) {
    yyjson_doc *doc; Json *j; size_t n = strlen(s);
    if (n > 32UL * 1024UL * 1024UL) return NULL;
    doc = yyjson_read(s, n, YYJSON_READ_NUMBER_AS_RAW);
    if (!doc) return NULL;
    j = from_value(yyjson_doc_get_root(doc), 0); yyjson_doc_free(doc); return j;
}
static yyjson_mut_val *to_value(yyjson_mut_doc *doc, const Json *j) {
    yyjson_mut_val *v, *item, *key; const Json *p; int ok;
    if (!j || j->type == J_NULL) v = yyjson_mut_null(doc);
    else if (j->type == J_BOOL) v = yyjson_mut_bool(doc, !strcmp(j->s, "true"));
    else if (j->type == J_NUM) v = yyjson_mut_rawcpy(doc, j->s);
    else if (j->type == J_STR) v = yyjson_mut_strcpy(doc, j->s);
    else {
        v = j->type == J_OBJ ? yyjson_mut_obj(doc) : yyjson_mut_arr(doc);
        if (!v) goto oom;
        for (p = j->child; p; p = p->next) {
            item = to_value(doc, p);
            if (j->type == J_OBJ) {
                key = yyjson_mut_strcpy(doc, p->key); if (!key) goto oom;
                ok = yyjson_mut_obj_add(v, key, item);
            } else ok = yyjson_mut_arr_append(v, item);
            if (!ok) goto oom;
        }
    }
    if (v) return v;
oom: fputs("out of memory\n", stderr); exit(1);
}
char *jdump(const Json *j) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL); char *s;
    if (!doc) { fputs("out of memory\n", stderr); exit(1); }
    yyjson_mut_doc_set_root(doc, to_value(doc, j));
    /* Shells can produce arbitrary bytes. Invalid UTF-8 becomes U+FFFD. */
    s = yyjson_mut_write(doc, YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    yyjson_mut_doc_free(doc);
    if (!s) { fputs("JSON serialization failed\n", stderr); exit(1); }
    return s;
}
Json *jcopy(const Json *j) {
    Json *v, *p;
    if (!j) return jnew(J_NULL);
    v = jnew(j->type); if (j->s) v->s = xstrdup(j->s);
    for (p = j->child; p; p = p->next) { Json *child = jcopy(p); if (p->key) child->key = xstrdup(p->key); jadd(v, child); }
    return v;
}

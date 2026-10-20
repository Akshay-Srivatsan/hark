/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_JSON_H
#define HARK_JSON_H
#include <stddef.h>
/* Small owning JSON tree. Children and object keys belong to their parent. */
typedef struct Json Json;
struct Json { int type; char *s, *key; Json *child, *next; };
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
Json *jnew(int type);
Json *jstr(const char *s);
Json *jnum(unsigned long n);
Json *jbool(int b);
void jadd(Json *parent, Json *value);
void jset(Json *obj, const char *key, Json *value);
Json *jget(const Json *obj, const char *key);
const char *jtext(const Json *j);
int jis(const Json *obj, const char *key, const char *value);
Json *jparse(const char *s);
char *jdump(const Json *j);
Json *jcopy(const Json *j);
void jfree(Json *j);
#endif

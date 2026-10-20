/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "auth.h"
#include "net.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef HARK_AUTH_ISSUER
#define HARK_AUTH_ISSUER "https://auth.openai.com"
#endif
#define ISSUER HARK_AUTH_ISSUER
#define CLIENT "app_EMoamEEZ73f0CkXaXp7hrann"
/* This is the public Codex client, not an application secret. */
static Json *post(const char *path, const char *type, const char *body) {
    char url[256]; Http r; Json *j; snprintf(url, sizeof url, "%s%s", ISSUER, path);
    r = http_post(url, NULL, NULL, type, body, 60); j = http_json(&r, NULL); http_free(&r); return j;
}
/* JWTs are decoded only to route the authenticated account, never to verify trust. */
static Json *jwt_payload(const char *token) {
    const char *p = strchr(token, '.'); char *s; size_t n = 0; unsigned acc = 0, bits = 0; Json *j;
    if (!p) return NULL;
    p++; s = xmalloc(strlen(p) + 1);
    while (*p && *p != '.') {
        const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        const char *v = strchr(alphabet, *p++);
        if (!v) { free(s); return NULL; }
        acc = (acc << 6) | (unsigned)(v - alphabet); bits += 6;
        if (bits >= 8) { bits -= 8; s[n++] = (char)((acc >> bits) & 255); }
    }
    s[n] = 0; j = memchr(s, 0, n) ? NULL : jparse(s); free(s); return j;
}
static void annotate(Json *j, const Json *old) {
    Json *jwt = jwt_payload(jtext(jget(j, "id_token"))), *value;
    if (!jwt) jwt = jwt_payload(jtext(jget(j, "access_token")));
    value = jget(jget(jwt, "https://api.openai.com/auth"), "chatgpt_account_id");
    if (value && value->type == J_STR) jset(j, "account_id", jcopy(value));
    else if (old && jget(old, "account_id")) jset(j, "account_id", jcopy(jget(old, "account_id")));
    if (!jget(j, "refresh_token") && old && jget(old, "refresh_token")) jset(j, "refresh_token", jcopy(jget(old, "refresh_token")));
    value = jget(j, "expires_in");
    if (value && value->type == J_NUM) {
        unsigned long seconds = strtoul(value->s, NULL, 10); if (seconds > 864000) seconds = 3600;
        jset(j, "expires_at", jnum((unsigned long)time(NULL) + seconds));
    } else jset(j, "expires_at", jnum((unsigned long)time(NULL) + 3600));
    jfree(jwt);
}
static int save(const char *path, const Json *j) {
    char *s = jdump(j); int ok = host_write_secret(path, s); free(s);
    if (!ok) fputs("cannot securely save credentials; check auth file's parent directory\n", stderr);
    return ok;
}
/* Percent-encode everything except RFC 3986 unreserved characters. */
static void form_pair(char **s, const char *key, const char *value) {
    static const char hex[] = "0123456789ABCDEF"; size_t n = strlen(*s), i;
    *s = xrealloc(*s, n + strlen(key) + 3 * strlen(value) + 3);
    if (n) (*s)[n++] = '&';
    strcpy(*s + n, key); n += strlen(key); (*s)[n++] = '=';
    for (i = 0; value[i]; i++) {
        unsigned char ch = (unsigned char)value[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || strchr("-._~", ch)) (*s)[n++] = (char)ch;
        else { (*s)[n++] = '%'; (*s)[n++] = hex[ch >> 4]; (*s)[n++] = hex[ch & 15]; }
    }
    (*s)[n] = 0;
}
int auth_login(const char *path) {
    Json *uc = post("/api/accounts/deviceauth/usercode", "application/json", "{\"client_id\":\"" CLIENT "\"}"), *poll, *j = NULL, *v;
    char *body, *form; Http r; unsigned interval = 5; time_t start = time(NULL);
    if (!uc) return 0;
    if (!*jtext(jget(uc, "user_code")) && jget(uc, "usercode")) jset(uc, "user_code", jcopy(jget(uc, "usercode")));
    if (!*jtext(jget(uc, "device_auth_id")) || !*jtext(jget(uc, "user_code"))) { jfree(uc); return 0; }
    v = jget(uc, "interval"); if (v && (v->type == J_STR || v->type == J_NUM)) { unsigned long x = strtoul(v->s, NULL, 10); if (x >= 1 && x <= 60) interval = (unsigned)x; }
    fputs("Open " ISSUER "/codex/device and enter: ", stdout); host_print(jtext(jget(uc, "user_code"))); puts("\nWaiting for login (up to 15 minutes)...");
    poll = jnew(J_OBJ); jset(poll, "device_auth_id", jcopy(jget(uc, "device_auth_id"))); jset(poll, "user_code", jcopy(jget(uc, "user_code"))); body = jdump(poll); jfree(poll); jfree(uc);
    while (difftime(time(NULL), start) < 900 && !host_interrupted()) {
        r = http_post(ISSUER "/api/accounts/deviceauth/token", NULL, NULL, "application/json", body, 60);
        if (http_ok(&r)) { j = http_json(&r, NULL); http_free(&r); break; }
        if (host_interrupted()) { http_free(&r); break; }
        if (r.status != 403 && r.status != 404) { http_report("device login failed", &r, NULL, NULL); http_free(&r); break; }
        http_free(&r); host_sleep(interval);
    }
    free(body);
    if (!j) { if (!host_interrupted()) fputs("device login failed or expired\n", stderr); return 0; }
    if (!*jtext(jget(j, "authorization_code")) || !*jtext(jget(j, "code_verifier"))) { jfree(j); return 0; }
    form = xstrdup("");
    form_pair(&form, "grant_type", "authorization_code"); form_pair(&form, "client_id", CLIENT);
    form_pair(&form, "redirect_uri", ISSUER "/deviceauth/callback");
    form_pair(&form, "code", jtext(jget(j, "authorization_code"))); form_pair(&form, "code_verifier", jtext(jget(j, "code_verifier")));
    jfree(j); j = post("/oauth/token", "application/x-www-form-urlencoded", form); free(form);
    if (!j || !*jtext(jget(j, "access_token")) || !*jtext(jget(j, "refresh_token"))) { jfree(j); return 0; }
    annotate(j, NULL); { int ok = save(path, j); jfree(j); if (ok) puts("Logged in. Saved login will be used for OpenAI when no API key is configured."); return ok; }
}
int auth_try_load(Auth *a, const char *path) {
    char *s = host_read_secret(path); a->path = xstrdup(path); a->tokens = s ? jparse(s) : NULL; free(s);
    if (!a->tokens || !*jtext(jget(a->tokens, "access_token"))) { auth_free(a); return 0; }
    return 1;
}
int auth_load(Auth *a, const char *path) {
    if (auth_try_load(a, path)) return 1;
    fputs("no safe credentials found; run hark login first\n", stderr); return 0;
}
const char *auth_key(Auth *a) { return jtext(jget(a->tokens, "access_token")); }
const char *auth_account(Auth *a) { return jtext(jget(a->tokens, "account_id")); }
int auth_refresh(Auth *a, int force) {
    Json *exp = jget(a->tokens, "expires_at"), *j; char *body;
    if (!force && exp && exp->type == J_NUM && strtod(exp->s, NULL) > (double)time(NULL) + 60) return 1;
    if (!*jtext(jget(a->tokens, "refresh_token"))) { fputs("login has no refresh token; log in again\n", stderr); return 0; }
    body = xstrdup("");
    form_pair(&body, "grant_type", "refresh_token"); form_pair(&body, "client_id", CLIENT); form_pair(&body, "refresh_token", jtext(jget(a->tokens, "refresh_token")));
    j = post("/oauth/token", "application/x-www-form-urlencoded", body); free(body);
    if (!j || !*jtext(jget(j, "access_token"))) { jfree(j); if (!host_interrupted()) fputs("refresh failed; log in again\n", stderr); return 0; }
    annotate(j, a->tokens);
    /* Keep rotated tokens in memory even if persisting fails. */
    jfree(a->tokens); a->tokens = j; return save(a->path, j);
}
void auth_free(Auth *a) { jfree(a->tokens); free(a->path); memset(a, 0, sizeof *a); }

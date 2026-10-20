/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_NET_H
#define HARK_NET_H
#include "json.h"
typedef struct { char *body; long status; } Http;
Http http_post(const char *url, const char *key, const char *account,
               const char *content_type, const char *body, unsigned timeout);
Http http_get(const char *url, const char *key, const char *account, unsigned timeout);
void http_free(Http *r);
int http_ok(const Http *r);
/* Parse a successful JSON object; otherwise report the failure (see http_report). */
Json *http_json(const Http *r, const char *secret);
/* Print "WHAT (HTTP n[, NOTE])" and the provider's error message, if any: escaped,
 * truncated, and with SECRET (the request's credential, if any) redacted. */
void http_report(const char *what, const Http *r, const char *note, const char *secret);
/* Accept a JSON response, or the final response.completed SSE event. */
Json *response_json(const char *body);
#endif

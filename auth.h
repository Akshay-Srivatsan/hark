/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_AUTH_H
#define HARK_AUTH_H
#include "json.h"
typedef struct { Json *tokens; char *path; } Auth;
int auth_login(const char *path);
int auth_load(Auth *a, const char *path);
int auth_try_load(Auth *a, const char *path);
int auth_refresh(Auth *a, int force);
const char *auth_key(Auth *a);
const char *auth_account(Auth *a);
void auth_free(Auth *a);
#endif

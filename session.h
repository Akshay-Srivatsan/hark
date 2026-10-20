/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_SESSION_H
#define HARK_SESSION_H
#include "json.h"
/* Conversation transcripts for `hark resume`, one directory per working
 * directory under the per-user state directory. Nothing is ever stored in the
 * working directory itself, which may be untrusted. */
void session_start(const char *cwd);
/* Lock and read the most recent session for cwd; NULL (with a message) if none. */
Json *session_resume(const char *cwd);
/* Write the context (adds "cwd"); a no-op until there is history to save. */
int session_save(Json *context);
int session_saved(void);
void session_end(void);
#endif

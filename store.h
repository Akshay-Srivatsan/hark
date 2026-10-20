/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_STORE_H
#define HARK_STORE_H
#include "json.h"
/* store_read loads the startup file; store_write refuses it and any .harkrc. */
char *store_read(const char *path, int *ok);
int store_write(const char *path, const char *text);
char *store_text(const char *path, size_t limit);
void store_close(void);
/* Exclusive advisory lock on PATH.lock, held until store_unlock; NULL if busy. */
typedef struct StoreLock StoreLock;
StoreLock *store_lock(const char *path);
void store_unlock(StoreLock *lock);
#endif

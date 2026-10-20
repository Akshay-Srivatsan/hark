/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_LINE_H
#define HARK_LINE_H
/* Interactive prompt only; piped input and approval answers use plain stdio. */
char *line_read(const char *prompt); /* PROMPT precedes the "> " marker. */
#endif

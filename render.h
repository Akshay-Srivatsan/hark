/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef HARK_RENDER_H
#define HARK_RENDER_H
/* Terminal presentation. Color means ANSI escape sequences; markdown means model
 * text is laid out as Markdown (styled when color is on, else spacing only).
 * Untrusted text is still printed through host_print, so only Hark emits escapes. */
enum { MODE_NEVER, MODE_ALWAYS, MODE_AUTO };
/* always|yes|force, never|no|none, auto|tty|if-tty; -1 if invalid. */
int render_mode(const char *value);
const char *render_mode_name(int mode);
void render_setup(int color, int markdown); /* Modes; auto checks stdout. */
int render_color(void);

typedef enum { STYLE_COMMAND, STYLE_OK, STYLE_FAIL, STYLE_WARN, STYLE_JOB, STYLE_NOTE, STYLE_THINKING } Style;
void render_style(Style style); /* No-ops without color. */
void render_reset(void);

/* Model text, possibly in pieces; with markdown it is rendered a line at a time. */
void render_text(const char *text);
/* Reasoning text (show_thinking), dimmed when color is on. */
void render_thinking(const char *text);
/* Finish a reply: flush any partial line and end with a newline if needed. */
void render_end(void);
#endif

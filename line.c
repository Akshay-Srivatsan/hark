/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "line.h"
#include "vendor/isocline/include/isocline.h"
#include <stdio.h>
#include <stdlib.h>

static void complete_commands(ic_completion_env_t *env, const char *prefix) {
    static const char *commands[] = {
        "/approval", "/approval model", "/approval ask", "/approval deny", "/approval allow",
        "/review-model ", "/review-model default", "/models", "/model ", "/speed ", "/thinking ", "/status", "/jobs", "/kill ", "/help",
        "/compact", "/reset", "/quit", "/set ", "/export ", "/context ", "/save ", "/include ", "/import ", "/prompt ",
        "/speed auto", "/speed standard", "/speed fast", "/speed flex", "/speed ultrafast",
        "/thinking default", "/thinking none", "/thinking minimal", "/thinking low",
        "/thinking medium", "/thinking high", "/thinking xhigh", "/thinking max", NULL
    };
    if (*prefix == '/') (void)ic_add_completions(env, prefix, commands);
}
static bool entire_line(const char *s, long len) { (void)s; return len > 0; }
static void complete(ic_completion_env_t *env, const char *prefix) {
    if (*prefix == '/') ic_complete_word(env, prefix, complete_commands, entire_line);
}
char *line_read(void) {
    static int initialized; char *s;
    if (!initialized) {
        ic_set_history(NULL, 200); /* Session memory only: never save prompts. */
        ic_set_prompt_marker("> ", "... ");
        ic_set_default_completer(complete, NULL);
        ic_enable_color(false); ic_enable_beep(false); ic_enable_hint(false);
        ic_enable_highlight(false); ic_enable_brace_matching(false);
        ic_enable_brace_insertion(false); ic_enable_inline_help(false);
        initialized = 1;
    }
    s = ic_readline("");
    if (s && !*s && feof(stdin)) { free(s); return NULL; }
    return s;
}

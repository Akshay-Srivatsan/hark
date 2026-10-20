/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "render.h"
#include "platform.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int color, markdown;
/* Per reply: what was printed last, whether the cursor is at a line start, and
 * the Markdown state (a partial line, an open code fence, the previous line). */
static int thinking, at_start = 1, in_code, prev_blank = 1, lines;
static char *line; static size_t used, cap;
/* Consecutive table rows, held until the table ends so columns can be aligned. */
#define TABLE_ROWS 512
static char *rows[TABLE_ROWS]; static size_t nrows;

int render_mode(const char *v) {
    static const char *const names[] = {"never", "always", "auto"};
    size_t i;
    for (i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(v, names[i])) return (int)i;
    return -1;
}
const char *render_mode_name(int mode) { return mode == MODE_ALWAYS ? "always" : mode == MODE_AUTO ? "auto" : "never"; }
void render_setup(int c, int m) {
    const char *term = getenv("TERM"), *no_color = getenv("NO_COLOR"); int tty = host_stdout_tty();
    /* auto follows ls and the NO_COLOR convention. */
    color = c == MODE_ALWAYS || (c == MODE_AUTO && tty && !(no_color && *no_color) && !(term && !strcmp(term, "dumb")));
    markdown = m == MODE_ALWAYS || (m == MODE_AUTO && tty);
    if (color) host_enable_ansi();
}
int render_color(void) { return color; }

#define RESET "\033[0m"
#define DIM "\033[2m"
static const char *const styles[] = {"\033[1;36m", "\033[32m", "\033[31m", "\033[33m", "\033[35m", DIM, "\033[2;3m"};
static void sgr(const char *code) { if (color) fputs(code, stdout); }
void render_style(Style style) { sgr(styles[style]); }
void render_reset(void) { sgr(RESET); }

/* Untrusted text always goes through host_print. */
static void put(const char *s, size_t n) {
    char *t;
    if (!n) return;
    t = xmalloc(n + 1); memcpy(t, s, n); t[n] = 0; host_print(t); free(t);
    at_start = s[n - 1] == '\n';
}
static void styled(const char *code, const char *s, size_t n) { sgr(code); put(s, n); sgr(RESET); }
static int boundary(char c) { return !c || strchr(" \t()[]{}<>.,;:!?'\"", c) != NULL; }
/* Offset of MARK (length k) at or after FROM, or n. */
static size_t find(const char *s, size_t from, size_t n, const char *mark, size_t k) {
    for (; from + k <= n; from++) if (!strncmp(s + from, mark, k)) return from;
    return n;
}
/* Inline spans keep their markers: `code`, **bold**, *italic* or _italic_, and [link](url). */
static void inline_md(const char *s, size_t n) {
    size_t i = 0, start = 0, end;
    while (i < n) {
        char c = s[i];
        if (c == '`' && (end = find(s, i + 1, n, "`", 1)) < n) {
            put(s + start, i - start); styled("\033[33m", s + i, end + 1 - i); i = start = end + 1; continue;
        }
        if (c == '*' && i + 1 < n && s[i + 1] == '*' && (end = find(s, i + 2, n, "**", 2)) < n && end > i + 2) {
            put(s + start, i - start); styled("\033[1m", s + i, end + 2 - i); i = start = end + 2; continue;
        }
        if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] != ' ' && (!i || boundary(s[i - 1]))) {
            char mark[2]; mark[0] = c; mark[1] = 0; end = i + 1;
            while ((end = find(s, end, n, mark, 1)) < n && !(s[end - 1] != ' ' && boundary(end + 1 < n ? s[end + 1] : 0))) end++;
            if (end < n && end > i + 1) { put(s + start, i - start); styled("\033[3m", s + i, end + 1 - i); i = start = end + 1; continue; }
        }
        if (c == '[' && (end = find(s, i + 1, n, "](", 2)) < n) {
            size_t close = find(s, end + 2, n, ")", 1);
            if (close < n) { put(s + start, i - start); styled("\033[4m", s + i, end + 1 - i); styled(DIM, s + end + 1, close - end); i = start = close + 1; continue; }
        }
        i++;
    }
    put(s + start, n - start);
}
static int rule(const char *s, size_t n) {
    size_t i, count = 0; char mark = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == ' ') continue;
        if (!strchr("-*_", s[i]) || (mark && s[i] != mark)) return 0;
        mark = s[i]; count++;
    }
    return count >= 3;
}
/* One complete line, without its newline. */
static void block(const char *s, size_t n) {
    size_t k = 0, j;
    while (k < n && s[k] == ' ') k++;
    if (k < 4 && n - k >= 3 && (!strncmp(s + k, "```", 3) || !strncmp(s + k, "~~~", 3))) { in_code = !in_code; styled(DIM, s, n); return; }
    if (in_code) { if (!color) fputs("    ", stdout); styled("\033[32m", s, n); return; }
    for (j = k; j < n && j < k + 6 && s[j] == '#'; j++) {}
    if (j > k && (j == n || s[j] == ' ')) {
        if (lines && !prev_blank) putchar('\n'); /* A little space before headings. */
        styled("\033[1;34m", s, n); return;
    }
    if (k < n && s[k] == '>') { put(s, k); styled(DIM, s + k, 1); inline_md(s + k + 1, n - k - 1); return; }
    if (rule(s, n)) { styled(DIM, s, n); return; }
    j = k; while (j < n && s[j] >= '0' && s[j] <= '9') j++;
    if ((k < n && strchr("-*+", s[k]) && k + 1 < n && s[k + 1] == ' ') || (j > k && j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ')) {
        size_t marker = j > k ? j + 1 : k + 1;
        put(s, k); styled("\033[36m", s + k, marker - k); inline_md(s + marker, n - marker); return;
    }
    inline_md(s, n);
}
/* Display width: UTF-8 characters (wide characters count as one). */
static size_t width(const char *s, size_t n) { size_t w = 0, i; for (i = 0; i < n; i++) w += ((unsigned char)s[i] & 0xc0) != 0x80; return w; }
static int table_row(const char *s, size_t n) { size_t k = 0; while (k < n && k < 4 && s[k] == ' ') k++; return k < 4 && k < n && s[k] == '|'; }
/* Split a row into cells at pipes outside `code` and not escaped. */
static size_t cells(const char *s, size_t n, const char **start, size_t *len, size_t max) {
    size_t i = 0, count = 0, from; int code = 0;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '|') i++;
    from = i;
    for (; i <= n && count < max; i++) {
        if (i < n && s[i] == '`') code = !code;
        if (i == n || (s[i] == '|' && !code && (i == 0 || s[i - 1] != '\\'))) {
            size_t a = from, b = i;
            while (a < b && s[a] == ' ') a++;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r')) b--;
            if (!(i == n && a == b && count)) { start[count] = s + a; len[count] = b - a; count++; } /* Drop the empty cell after a trailing pipe. */
            from = i + 1;
        }
    }
    return count;
}
static int delimiter(const char **start, const size_t *len, size_t count) {
    size_t i, j;
    for (i = 0; i < count; i++) {
        if (!len[i]) return 0;
        for (j = 0; j < len[i]; j++) if (!strchr("-: ", start[i][j])) return 0;
    }
    return count > 0;
}
#define TABLE_COLS 64
static void flush_table(void) {
    const char *start[TABLE_COLS]; size_t len[TABLE_COLS], widths[TABLE_COLS], i, j, ncols = 0, count; char align[TABLE_COLS];
    if (!nrows) return;
    count = nrows > 1 ? cells(rows[1], strlen(rows[1]), start, len, TABLE_COLS) : 0;
    if (nrows < 2 || !delimiter(start, len, count)) { /* Not a table after all. */
        for (i = 0; i < nrows; i++) { inline_md(rows[i], strlen(rows[i])); putchar('\n'); free(rows[i]); }
        nrows = 0; at_start = 1; return;
    }
    memset(align, 'l', sizeof align); /* Columns beyond the delimiter row are left-aligned. */
    for (j = 0; j < count; j++) align[j] = start[j][len[j] - 1] == ':' ? (start[j][0] == ':' ? 'c' : 'r') : 'l';
    memset(widths, 0, sizeof widths);
    for (i = 0; i < nrows; i++) {
        if (i == 1) continue;
        count = cells(rows[i], strlen(rows[i]), start, len, TABLE_COLS);
        if (count > ncols) ncols = count;
        for (j = 0; j < count; j++) if (width(start[j], len[j]) > widths[j]) widths[j] = width(start[j], len[j]);
    }
    for (j = 0; j < ncols; j++) if (widths[j] < 3) widths[j] = 3;
    for (i = 0; i < nrows; i++) {
        count = i == 1 ? 0 : cells(rows[i], strlen(rows[i]), start, len, TABLE_COLS);
        for (j = 0; j < ncols; j++) {
            size_t w = j < count ? width(start[j], len[j]) : 0, pad = widths[j] - w, left = 0, k;
            styled(DIM, j ? " | " : "| ", j ? 3 : 2);
            if (i == 1) { /* Delimiter row, redrawn to the column width. */
                char *dashes = xmalloc(widths[j] + 1); memset(dashes, '-', widths[j]); dashes[widths[j]] = 0;
                if (align[j] != 'l') dashes[widths[j] - 1] = ':';
                if (align[j] == 'c') dashes[0] = ':';
                styled(DIM, dashes, widths[j]); free(dashes); continue;
            }
            if (align[j] == 'r') left = pad; else if (align[j] == 'c') left = pad / 2;
            for (k = 0; k < left; k++) putchar(' ');
            if (i == 0) sgr("\033[1m");
            if (j < count) inline_md(start[j], len[j]);
            if (i == 0) sgr(RESET);
            for (k = left; k < pad; k++) putchar(' ');
        }
        styled(DIM, " |", 2); putchar('\n');
    }
    for (i = 0; i < nrows; i++) free(rows[i]);
    nrows = 0; at_start = 1; prev_blank = 0;
}
static void finish_line(int newline) {
    if (!in_code && table_row(line, used) && nrows < TABLE_ROWS) {
        rows[nrows] = xmalloc(used + 1); memcpy(rows[nrows], line, used); rows[nrows][used] = 0; nrows++;
        used = 0; lines++; return;
    }
    flush_table();
    block(line, used);
    if (newline) { putchar('\n'); at_start = 1; }
    prev_blank = used == 0; lines++; used = 0;
}
void render_thinking(const char *s) {
    if (!*s) return;
    sgr(styles[STYLE_THINKING]); put(s, strlen(s)); sgr(RESET); thinking = 1; fflush(stdout);
}
void render_text(const char *s) {
    size_t n = strlen(s), i;
    if (!n) return;
    /* Separate the reply from any reasoning shown before it. */
    if (thinking) { if (!at_start) putchar('\n'); putchar('\n'); at_start = 1; thinking = 0; }
    if (!markdown) { put(s, n); fflush(stdout); return; }
    for (i = 0; i < n; i++) {
        if (s[i] == '\n') { finish_line(1); continue; }
        if (used + 1 >= cap) { cap = cap ? cap * 2 : 256; line = xrealloc(line, cap); }
        line[used++] = s[i];
    }
    fflush(stdout);
}
void render_end(void) {
    if (used) finish_line(0);
    flush_table();
    if (!at_start) putchar('\n');
    sgr(RESET); fflush(stdout);
    at_start = prev_blank = 1; thinking = in_code = 0; lines = 0;
}

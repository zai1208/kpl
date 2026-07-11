/*
 * kpp - KPL Preprocessor
 *
 * Stage 1 of the KPL toolchain (see spec/COMPILER.md section 2).
 *
 *   - Flattens INCLUDE "file" directives into a single line stream,
 *     silently skipping any file that has already been visited so that
 *     cyclic/diamond includes can't cause infinite recursion.
 *   - Indexes every PROC and STRUCT block found in the flattened stream.
 *   - Walks the call graph starting from `kpl_main` (the conventional
 *     kernel entry point - see spec/COMPILER.md 4.1, ENTRY(kpl_main)) and
 *     from any call that appears outside of a PROC body, transitively
 *     marking every reachable PROC as used.
 *   - Emits a metadata anchor comment before every STRUCT (always kept)
 *     and before every *used* PROC. Unused PROCs are dropped: this is the
 *     tree-shaking / dead-code-elimination pass.
 *
 * Design notes / places where the spec was underspecified and a concrete
 * choice had to be made:
 *
 *   1. spec/COMPILER.md 2.2 says Pass 1 records a "stream offset"; this
 *      implementation records a line index instead, which is simpler and
 *      equivalent for a strictly line-oriented tool.
 *   2. The spec says Pass 2 "tracks active usage" from the root file but
 *      doesn't say whether that's transitive. Non-transitive tree-shaking
 *      would keep a called proc but discard everything *it* calls, which
 *      isn't real dead-code elimination, so this implementation computes
 *      the full transitive closure of the call graph (a worklist/BFS).
 *   3. Nothing in KPL source ever *calls* the kernel entry point - it's
 *      invoked externally via ENTRY(kpl_main) in linker.ld - so it would
 *      always be tree-shaken away as "unused" without a special case.
 *      This implementation always treats a PROC literally named
 *      `kpl_main` as a reachability root.
 *
 * Written in strict ISO C99 with fixed-size buffers only, no dynamic
 * allocation, so it builds cleanly under slimcc/kefir as well as gcc/clang.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#define MAX_INCLUDES 256
#define MAX_PATH_LEN 512
#define MAX_LINE_LEN 1024
#define MAX_LINES    65536
#define MAX_BLOCKS   4096
#define MAX_NAME_LEN 128
#define MAX_CALLS    16384
#define MAX_WORKLIST 4096

typedef struct {
    char text[MAX_LINE_LEN];
    char file[MAX_PATH_LEN];
    int  src_line;
} Line;

static Line g_lines[MAX_LINES];
static int  g_line_count = 0;
static int  g_in_block[MAX_LINES];

static char g_visited[MAX_INCLUDES][MAX_PATH_LEN];
static int  g_visited_count = 0;

typedef enum { BLOCK_PROC, BLOCK_STRUCT } BlockType;

typedef struct {
    BlockType type;
    char name[MAX_NAME_LEN];
    char file[MAX_PATH_LEN];
    int  src_line;  /* original line number of the decl, for the anchor */
    int  start;     /* index into g_lines of the PROC/STRUCT line */
    int  end;       /* index into g_lines of the matching '}' line */
    int  used;      /* only meaningful for BLOCK_PROC */
} Block;

static Block g_blocks[MAX_BLOCKS];
static int   g_block_count = 0;

typedef struct {
    char caller[MAX_NAME_LEN]; /* "" if the call site is outside any PROC */
    char callee[MAX_NAME_LEN];
} CallSite;

static CallSite g_calls[MAX_CALLS];
static int      g_call_count = 0;

static char g_worklist[MAX_WORKLIST][MAX_NAME_LEN];
static int  g_worklist_count = 0;

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int is_ident_char(int c) { return isalnum((unsigned char)c) || c == '_'; }
static int is_ident_start(int c) { return isalpha((unsigned char)c) || c == '_'; }

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Does `p` start with keyword `kw` followed by a non-identifier char
 * (so "PROC" doesn't also match inside a hypothetical "PROCEDURE")? */
static int starts_with_word(const char *p, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0) return 0;
    return !is_ident_char((unsigned char)p[n]);
}

/* Resolves an INCLUDE "path" relative to the directory of the file that
 * contains the directive (mirroring C's #include semantics), so a project
 * can be built from any cwd as long as the root file's own path is
 * correct. Absolute paths (leading '/') are left untouched. */
static void resolve_include_path(const char *base_file, const char *inc_path, char *out, size_t outsz) {
    if (inc_path[0] == '/') {
        strncpy(out, inc_path, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    const char *slash = strrchr(base_file, '/');
    if (!slash) {
        strncpy(out, inc_path, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    size_t dirlen = (size_t)(slash - base_file) + 1; /* include the '/' */
    if (dirlen >= outsz) dirlen = outsz - 1;
    memcpy(out, base_file, dirlen);
    size_t remain = outsz - dirlen;
    strncpy(out + dirlen, inc_path, remain - 1);
    out[dirlen + remain - 1] = '\0';
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "kpp: error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* Copies `in` into `out` truncated at the first top-level comment
 * character (';', see LANGUAGE.md 1.2), so structural scans (brace
 * counting, call-site detection) never get confused by braces or
 * parens that only appear inside a comment. */
static void strip_comment(const char *in, char *out, size_t outsz) {
    size_t n = 0;
    for (; *in && *in != ';' && n < outsz - 1; in++) out[n++] = *in;
    out[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Pass 0: INCLUDE flattening                                         */
/* ------------------------------------------------------------------ */

static void flatten(const char *filename) {
    int i;
    for (i = 0; i < g_visited_count; i++)
        if (strcmp(g_visited[i], filename) == 0) return; /* cycle/dup: skip silently */

    if (g_visited_count >= MAX_INCLUDES) die("too many included files (limit %d)", MAX_INCLUDES);
    strncpy(g_visited[g_visited_count], filename, MAX_PATH_LEN - 1);
    g_visited[g_visited_count][MAX_PATH_LEN - 1] = '\0';
    g_visited_count++;

    FILE *f = fopen(filename, "r");
    if (!f) die("cannot open '%s'", filename);

    char buf[MAX_LINE_LEN];
    int lineno = 0;
    while (fgets(buf, sizeof(buf), f)) {
        lineno++;
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';

        const char *p = skip_ws(buf);
        if (starts_with_word(p, "INCLUDE")) {
            const char *q = skip_ws(p + 7);
            if (*q != '"') die("%s:%d: malformed INCLUDE directive (expected \"path\")", filename, lineno);
            q++;
            char path[MAX_PATH_LEN];
            int k = 0;
            while (*q && *q != '"' && k < MAX_PATH_LEN - 1) path[k++] = *q++;
            if (*q != '"') die("%s:%d: unterminated string in INCLUDE directive", filename, lineno);
            path[k] = '\0';
            char resolved[MAX_PATH_LEN];
            resolve_include_path(filename, path, resolved, sizeof resolved);
            flatten(resolved); /* recurse; our own `f`/lineno state is untouched */
            continue;
        }

        if (g_line_count >= MAX_LINES) die("too many lines (limit %d)", MAX_LINES);
        Line *L = &g_lines[g_line_count++];
        strncpy(L->text, buf, MAX_LINE_LEN - 1); L->text[MAX_LINE_LEN - 1] = '\0';
        strncpy(L->file, filename, MAX_PATH_LEN - 1); L->file[MAX_PATH_LEN - 1] = '\0';
        L->src_line = lineno;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Pass 1: index PROC / STRUCT blocks                                 */
/* ------------------------------------------------------------------ */

static void parse_name_after_keyword(const char *line, const char *keyword, char *out, size_t outsz) {
    const char *p = skip_ws(line);
    p += strlen(keyword);
    p = skip_ws(p);
    size_t i = 0;
    while (is_ident_char((unsigned char)*p) && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
}

/* Scans forward from `start`, counting braces (ignoring comments) until
 * the depth returns to zero after having gone positive at least once.
 * Returns the line index of the matching closing brace. */
static int scan_block_end(int start) {
    int depth = 0, seen_open = 0, i;
    for (i = start; i < g_line_count; i++) {
        char code[MAX_LINE_LEN];
        strip_comment(g_lines[i].text, code, sizeof code);
        const char *s = code;
        for (; *s; s++) {
            if (*s == '{') { depth++; seen_open = 1; }
            else if (*s == '}') { depth--; }
        }
        if (seen_open && depth <= 0) return i;
    }
    return g_line_count - 1; /* unterminated block: best effort, avoids OOB */
}

static void add_block(BlockType type, const char *name, const char *file, int src_line, int start, int end) {
    if (g_block_count >= MAX_BLOCKS) die("too many PROC/STRUCT blocks (limit %d)", MAX_BLOCKS);
    Block *b = &g_blocks[g_block_count++];
    b->type = type;
    strncpy(b->name, name, MAX_NAME_LEN - 1); b->name[MAX_NAME_LEN - 1] = '\0';
    strncpy(b->file, file, MAX_PATH_LEN - 1); b->file[MAX_PATH_LEN - 1] = '\0';
    b->src_line = src_line;
    b->start = start;
    b->end = end;
    b->used = 0;
}

static void find_blocks(void) {
    int i = 0;
    while (i < g_line_count) {
        const char *p = skip_ws(g_lines[i].text);
        if (starts_with_word(p, "PROC")) {
            char name[MAX_NAME_LEN];
            parse_name_after_keyword(g_lines[i].text, "PROC", name, sizeof name);
            int end = scan_block_end(i);
            add_block(BLOCK_PROC, name, g_lines[i].file, g_lines[i].src_line, i, end);
            i = end + 1;
        } else if (starts_with_word(p, "STRUCT")) {
            char name[MAX_NAME_LEN];
            parse_name_after_keyword(g_lines[i].text, "STRUCT", name, sizeof name);
            int end = scan_block_end(i);
            add_block(BLOCK_STRUCT, name, g_lines[i].file, g_lines[i].src_line, i, end);
            i = end + 1;
        } else {
            i++;
        }
    }
    for (i = 0; i < g_block_count; i++) {
        int j;
        for (j = g_blocks[i].start; j <= g_blocks[i].end; j++) g_in_block[j] = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Pass 2: find call sites (`identifier(` and ASM-style `call ident`) */
/* ------------------------------------------------------------------ */

static void record_call(const char *caller, const char *callee) {
    if (g_call_count >= MAX_CALLS) die("too many call sites (limit %d)", MAX_CALLS);
    CallSite *c = &g_calls[g_call_count++];
    strncpy(c->caller, caller, MAX_NAME_LEN - 1); c->caller[MAX_NAME_LEN - 1] = '\0';
    strncpy(c->callee, callee, MAX_NAME_LEN - 1); c->callee[MAX_NAME_LEN - 1] = '\0';
}

static void scan_calls_in_line(const char *caller, const char *text) {
    char buf[MAX_LINE_LEN];
    strip_comment(text, buf, sizeof buf);

    const char *s = buf;
    while (*s) {
        if (!is_ident_start((unsigned char)*s)) { s++; continue; }

        const char *start = s;
        while (is_ident_char((unsigned char)*s)) s++;
        size_t idlen = (size_t)(s - start);
        char id[MAX_NAME_LEN];
        if (idlen >= sizeof(id)) idlen = sizeof(id) - 1;
        memcpy(id, start, idlen);
        id[idlen] = '\0';

        if (strcmp(id, "call") == 0) {
            /* ASM-style `call identifier` (spec/COMPILER.md 6.4) */
            const char *q = skip_ws(s);
            if (is_ident_start((unsigned char)*q)) {
                const char *cstart = q;
                while (is_ident_char((unsigned char)*q)) q++;
                size_t clen = (size_t)(q - cstart);
                char cid[MAX_NAME_LEN];
                if (clen >= sizeof(cid)) clen = sizeof(cid) - 1;
                memcpy(cid, cstart, clen);
                cid[clen] = '\0';
                record_call(caller, cid);
            }
        } else {
            const char *after = skip_ws(s);
            if (*after == '(') record_call(caller, id);
        }
    }
}

static void scan_all_calls(void) {
    int k;
    /* interior lines of each PROC block, attributed to that proc */
    for (k = 0; k < g_block_count; k++) {
        if (g_blocks[k].type != BLOCK_PROC) continue;
        int j;
        for (j = g_blocks[k].start + 1; j < g_blocks[k].end; j++)
            scan_calls_in_line(g_blocks[k].name, g_lines[j].text);
    }
    /* genuinely loose top-level lines (outside any PROC/STRUCT), treated
     * as reachability roots since they execute unconditionally */
    int i;
    for (i = 0; i < g_line_count; i++)
        if (!g_in_block[i])
            scan_calls_in_line("", g_lines[i].text);
}

/* ------------------------------------------------------------------ */
/* Pass 3: transitive reachability (tree-shaking)                     */
/* ------------------------------------------------------------------ */

static Block *find_proc(const char *name) {
    int i;
    for (i = 0; i < g_block_count; i++)
        if (g_blocks[i].type == BLOCK_PROC && strcmp(g_blocks[i].name, name) == 0)
            return &g_blocks[i];
    return NULL;
}

static void mark_used(const char *name) {
    Block *b = find_proc(name);
    if (!b || b->used) return; /* unknown symbol (external/asm-only) or already queued */
    b->used = 1;
    if (g_worklist_count >= MAX_WORKLIST) die("call graph too deep (limit %d)", MAX_WORKLIST);
    strncpy(g_worklist[g_worklist_count], name, MAX_NAME_LEN - 1);
    g_worklist[g_worklist_count][MAX_NAME_LEN - 1] = '\0';
    g_worklist_count++;
}

static void compute_reachability(void) {
    int i;
    mark_used("kpl_main"); /* conventional kernel entry point, see header comment */
    for (i = 0; i < g_call_count; i++)
        if (g_calls[i].caller[0] == '\0') mark_used(g_calls[i].callee);

    int head = 0;
    while (head < g_worklist_count) {
        const char *cur = g_worklist[head++];
        for (i = 0; i < g_call_count; i++)
            if (strcmp(g_calls[i].caller, cur) == 0) mark_used(g_calls[i].callee);
    }
}

/* ------------------------------------------------------------------ */
/* Pass 4: emit flattened, shaken output                              */
/* ------------------------------------------------------------------ */

static Block *block_starting_at(int i) {
    int k;
    for (k = 0; k < g_block_count; k++)
        if (g_blocks[k].start == i) return &g_blocks[k];
    return NULL;
}

static void emit_output(void) {
    int i = 0;
    while (i < g_line_count) {
        Block *b = block_starting_at(i);
        if (b) {
            if (b->type == BLOCK_STRUCT || b->used) {
                printf("#;[%s]:[%s]:[%d]\n", b->file, b->name, b->src_line);
                int j;
                for (j = b->start; j <= b->end; j++) printf("%s\n", g_lines[j].text);
            }
            i = b->end + 1;
        } else {
            printf("%s\n", g_lines[i].text);
            i++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kpp <root.kpl>\n");
        return 1;
    }
    flatten(argv[1]);
    find_blocks();
    scan_all_calls();
    compute_reachability();
    emit_output();
    return 0;
}

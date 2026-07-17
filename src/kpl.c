/*
 * kpl - KPL Transpiler
 *
 * Stage 2 of the KPL toolchain (see spec/LANGUAGE.md and spec/COMPILER.md).
 * Reads a single flattened token stream on stdin (normally the output of
 * `kpp`) and emits deterministic System-V AMD64 NASM text.
 *
 * Also implements `kpl init`, the zero-config scaffolding command from
 * spec/COMPILER.md section 4.
 *
 * ------------------------------------------------------------------------
 * Design notes / places where the spec was underspecified and a concrete
 * choice had to be made (flagged here rather than silently invented):
 *
 *  1. Parameters are spilled from their ABI registers to their stack slots
 *     immediately after the prologue, so that a parameter behaves exactly
 *     like a local variable for every later reference (linear expressions,
 *     ASM `[name]` substitution, etc). The spec's codegen schema (4.3)
 *     doesn't show this explicitly, but without it params would be
 *     unreadable/unwritable from anywhere except the very first
 *     instruction, and ASM's `[var]` -> `[rbp-N]` rule (6.3) implies every
 *     named value already lives on the stack.
 *
 *  2. A `sub rsp, N` needs to know the whole frame size before the first
 *     body instruction, but locals are only discovered while walking the
 *     body. This implementation buffers a PROC's body instructions in a
 *     scratch buffer while it walks the source (assigning stack offsets
 *     as it goes), then once the final size is known emits
 *     prologue + buffered body + epilogue as one unit. This produces
 *     identical output to a two-pass compiler without a second parse.
 *
 *  3. STRUCT truly emits no assembly (per spec, "does not emit text"), so
 *     as written a STRUCT only records field offsets/size; there is no
 *     specified syntax for reading a struct field through a typed
 *     variable, so none is implemented. If field access is wanted, the
 *     natural extension point is here.
 *
 *  4. `RETURN` isn't in spec/LANGUAGE.md or spec/COMPILER.md, but it *was*
 *     in the original skeleton kpl.c, and without it there's no way to
 *     exit a PROC early from inside an `if`. It's kept as an early-exit
 *     statement emitting the same epilogue every closing `}` produces.
 *
 *  5. Comparisons use unsigned jump variants (jae/jbe) rather than signed
 *     (jge/jle) throughout, since every KPL primitive type is explicitly
 *     unsigned (spec/LANGUAGE.md section 2).
 *
 *  6. Register-name arguments passed directly to a call (e.g. `foo(rsi)`)
 *     are loaded into their ABI registers in left-to-right order without
 *     staging through temporaries first. If two such arguments alias
 *     (e.g. `foo(rsi, rdi)`, swapping registers), the second load can
 *     read an already-clobbered register. Every call example in the spec
 *     passes named variables, not bare registers, so this is a known,
 *     documented sharp edge rather than a silent miscompile risk.
 *
 * Written in strict ISO C99 with fixed-size buffers only, no dynamic
 * allocation, so it builds cleanly under slimcc/kefir as well as gcc/clang.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* limits                                                              */
/* ------------------------------------------------------------------ */

#define MAX_NAME_LEN  128
#define MAX_PATH_LEN  512
#define MAX_LINE_LEN  1024
#define MAX_LINES     65536
#define MAX_TOK       128

#define MAX_STRUCTS   128
#define MAX_FIELDS    64
#define MAX_LOCALS    256
#define MAX_GLOBALS   1024
#define MAX_NEST      128
#define MAX_CHAIN_HOPS 8

#define TEXT_BUF_SIZE (1 << 20)
#define DATA_BUF_SIZE (1 << 18)
#define BODY_BUF_SIZE (1 << 16)

/* ------------------------------------------------------------------ */
/* input line storage + location tracking                             */
/* ------------------------------------------------------------------ */

typedef struct { char text[MAX_LINE_LEN]; } RawLine;
static RawLine g_lines[MAX_LINES];
static int g_line_count = 0;
static int g_pos = 0;

static char g_cur_file[MAX_PATH_LEN] = "<stdin>";
static char g_cur_name[MAX_NAME_LEN] = "";
static int  g_cur_src_line = 0;

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

typedef enum { TY_U64, TY_U32, TY_PTR } Type;

static int type_size(Type t) { return (t == TY_U32) ? 4 : 8; }

static int type_from_str(const char *s, Type *out) {
    if (strcmp(s, "u64") == 0) { *out = TY_U64; return 1; }
    if (strcmp(s, "u32") == 0) { *out = TY_U32; return 1; }
    if (strcmp(s, "ptr") == 0) { *out = TY_PTR; return 1; }
    return 0;
}

typedef struct { char name[MAX_NAME_LEN]; Type type; int offset; int struct_id; } StructField;
typedef struct {
    char name[MAX_NAME_LEN];
    StructField fields[MAX_FIELDS];
    int field_count;
    int size;
} StructDef;
static StructDef g_structs[MAX_STRUCTS];
static int g_struct_count = 0;

/* Tracks every PROC name defined in this translation unit, and every
 * name ever used as a call target. At the end, any called name that was
 * never defined locally gets an `extern` declaration emitted for it, so
 * KPL code can call hand-written routines (e.g. a kstd driver written
 * directly in NASM) living in a separate object file. */
#define MAX_PROCS 2048
static char g_defined_procs[MAX_PROCS][MAX_NAME_LEN];
static int  g_defined_proc_count = 0;
static char g_called_names[MAX_PROCS][MAX_NAME_LEN];
static int  g_called_name_count = 0;

static int name_in_table(char table[][MAX_NAME_LEN], int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) if (strcmp(table[i], name) == 0) return 1;
    return 0;
}

static void record_defined_proc(const char *name) {
    if (name_in_table(g_defined_procs, g_defined_proc_count, name)) return;
    if (g_defined_proc_count >= MAX_PROCS) return; /* best effort; also caught by MAX_STRUCTS-style limits elsewhere */
    strncpy(g_defined_procs[g_defined_proc_count], name, MAX_NAME_LEN - 1);
    g_defined_procs[g_defined_proc_count][MAX_NAME_LEN - 1] = '\0';
    g_defined_proc_count++;
}

static void record_called_name(const char *name) {
    if (name_in_table(g_called_names, g_called_name_count, name)) return;
    if (g_called_name_count >= MAX_PROCS) return;
    strncpy(g_called_names[g_called_name_count], name, MAX_NAME_LEN - 1);
    g_called_names[g_called_name_count][MAX_NAME_LEN - 1] = '\0';
    g_called_name_count++;
}

typedef struct { char name[MAX_NAME_LEN]; Type type; } GlobalVar;
static GlobalVar g_globals[MAX_GLOBALS];
static int g_global_count = 0;

typedef struct { char name[MAX_NAME_LEN]; Type type; int offset; int struct_id; } LocalVar;
static LocalVar g_locals[MAX_LOCALS];
static int g_local_count = 0;
static int g_frame_size = 0;

typedef enum { OPND_IMM, OPND_REG, OPND_MEM, OPND_FIELD } OperandKind;
typedef struct {
    OperandKind kind;
    unsigned long imm;
    char reg[8];
    char memref[64];
    int hop_offsets[MAX_CHAIN_HOPS]; /* one dereference-and-add per chain hop */
    int hop_count;
    Type type;
} Operand;

typedef enum { BLK_IF, BLK_ELSE, BLK_LOOP } BlockKind;
typedef struct { BlockKind kind; int label_id; } CtrlBlock;
static CtrlBlock g_ctrl_stack[MAX_NEST];
static int g_ctrl_top = 0;
static int g_label_id = 0;

/* ------------------------------------------------------------------ */
/* output buffers                                                     */
/* ------------------------------------------------------------------ */

static char g_text_buf[TEXT_BUF_SIZE]; static size_t g_text_len = 0;
static char g_data_buf[DATA_BUF_SIZE]; static size_t g_data_len = 0;
static char g_body_buf[BODY_BUF_SIZE]; static size_t g_body_len = 0;

static void vbuf_emit(char *buf, size_t bufsz, size_t *len, const char *fmt, va_list ap) {
    if (*len + 1 >= bufsz) return; /* silently stop growing on overflow */
    int n = vsnprintf(buf + *len, bufsz - *len, fmt, ap);
    if (n > 0) {
        size_t written = (size_t)n;
        if (written > bufsz - *len - 1) written = bufsz - *len - 1;
        *len += written;
    }
}
static void text_emit(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vbuf_emit(g_text_buf, TEXT_BUF_SIZE, &g_text_len, fmt, ap); va_end(ap); }
static void data_emit(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vbuf_emit(g_data_buf, DATA_BUF_SIZE, &g_data_len, fmt, ap); va_end(ap); }
static void body_emit(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vbuf_emit(g_body_buf, BODY_BUF_SIZE, &g_body_len, fmt, ap); va_end(ap); }

/* ------------------------------------------------------------------ */
/* small string helpers                                               */
/* ------------------------------------------------------------------ */

static int is_ident_char(int c) { return isalnum((unsigned char)c) || c == '_'; }
static int is_ident_start(int c) { return isalpha((unsigned char)c) || c == '_'; }

static char *skip_ws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static int starts_with_word(const char *p, const char *kw) {
    size_t n = strlen(kw);
    if (strncmp(p, kw, n) != 0) return 0;
    return !is_ident_char((unsigned char)p[n]);
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end = '\0';
    return s;
}

static void die_loc(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "kpl: error: %s:%d: (in %s) ", g_cur_file, g_cur_src_line, g_cur_name[0] ? g_cur_name : "<top-level>");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* Splits `raw` into a comment-free code part and a trimmed comment part
 * (text following the first ';' - spec/LANGUAGE.md 1.2). Both outputs
 * are trimmed of surrounding whitespace. */
static void split_comment(const char *raw, char *code, size_t codesz, char *comment, size_t commentsz) {
    const char *semi = strchr(raw, ';');
    size_t clen = semi ? (size_t)(semi - raw) : strlen(raw);
    if (clen >= codesz) clen = codesz - 1;
    memcpy(code, raw, clen);
    code[clen] = '\0';
    char *ct = trim(code);
    if (ct != code) memmove(code, ct, strlen(ct) + 1);

    if (semi) {
        strncpy(comment, semi + 1, commentsz - 1);
        comment[commentsz - 1] = '\0';
        char *tt = trim(comment);
        if (tt != comment) memmove(comment, tt, strlen(tt) + 1);
    } else {
        comment[0] = '\0';
    }
}

static int split_ws(char *s, char toks[][MAX_TOK], int maxtoks) {
    int n = 0;
    char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (n < maxtoks) {
            if (len >= MAX_TOK) len = MAX_TOK - 1;
            memcpy(toks[n], start, len);
            toks[n][len] = '\0';
        }
        n++;
    }
    return n;
}

static int split_commas(char *s, char toks[][MAX_TOK], int maxtoks) {
    int n = 0;
    char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        char *start = p;
        while (*p && *p != ',') p++;
        char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        size_t len = (size_t)(end - start);
        if (n < maxtoks) {
            if (len >= MAX_TOK) len = MAX_TOK - 1;
            memcpy(toks[n], start, len);
            toks[n][len] = '\0';
        }
        n++;
        if (*p == ',') { p++; continue; }
        break;
    }
    return n;
}

/* Binary operators valid in an `a OP b` expression: the original
 * arithmetic four, plus bitwise AND/OR/XOR (one instruction each) and
 * the two shifts (two-character tokens, unlike every other operator
 * here - `split_ws` still hands them over whole since there's no
 * whitespace inside "<<"/">>", just like there's none inside any other
 * operator token). */
static int is_op_token(const char *s) {
    if (s[0] != '\0' && s[1] == '\0' && strchr("+-*/&|^", s[0]) != NULL) return 1;
    if (strcmp(s, "<<") == 0 || strcmp(s, ">>") == 0) return 1;
    return 0;
}

/* Prefix unary operators: a genuinely different shape from `a OP b`
 * (one operand, not two), kept as its own table/dispatch rather than
 * bolted onto is_op_token/emit_binop, since more unary operators are a
 * reasonable thing to add later without reshaping this one. */
static int is_unary_op_token(const char *s) { return strcmp(s, "~") == 0; }

static int parse_int_literal(const char *s, unsigned long *out) {
    if (!*s) return 0;
    char *end;
    unsigned long v = strtoul(s, &end, 0); /* base 0: handles "0x..." and decimal */
    if (end == s || *end != '\0') return 0;
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* registers                                                          */
/* ------------------------------------------------------------------ */

static const char *REGS64[] = {
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", NULL
};

static int is_register_name(const char *s) {
    int i;
    for (i = 0; REGS64[i]; i++) if (strcmp(s, REGS64[i]) == 0) return 1;
    return 0;
}

static const char *reg32_of(const char *r64) {
    static const char *r64tab[] = {"rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15"};
    static const char *r32tab[] = {"eax","ebx","ecx","edx","esi","edi","ebp","esp","r8d","r9d","r10d","r11d","r12d","r13d","r14d","r15d"};
    size_t i;
    for (i = 0; i < sizeof(r64tab) / sizeof(r64tab[0]); i++)
        if (strcmp(r64, r64tab[i]) == 0) return r32tab[i];
    return r64; /* unreachable for known registers */
}

/* ------------------------------------------------------------------ */
/* input stream: transparently absorbs kpp's `#;[file]:[name]:[line]` */
/* metadata anchors (spec/COMPILER.md 2.3) to drive error locations   */
/* ------------------------------------------------------------------ */

static int try_parse_anchor(const char *line) {
    if (strncmp(line, "#;[", 3) != 0) return 0;
    const char *p = line + 3;
    char file[MAX_PATH_LEN]; int fi = 0;
    while (*p && *p != ']' && fi < MAX_PATH_LEN - 1) file[fi++] = *p++;
    file[fi] = '\0';
    if (*p != ']') return 0;
    p++;
    if (*p != ':') return 0;
    p++;
    if (*p != '[') return 0;
    p++;
    char name[MAX_NAME_LEN]; int ni = 0;
    while (*p && *p != ']' && ni < MAX_NAME_LEN - 1) name[ni++] = *p++;
    name[ni] = '\0';
    if (*p != ']') return 0;
    p++;
    if (*p != ':') return 0;
    p++;
    if (*p != '[') return 0;
    p++;

    strncpy(g_cur_file, file, sizeof(g_cur_file) - 1); g_cur_file[sizeof(g_cur_file) - 1] = '\0';
    strncpy(g_cur_name, name, sizeof(g_cur_name) - 1); g_cur_name[sizeof(g_cur_name) - 1] = '\0';
    g_cur_src_line = atoi(p);
    return 1;
}

static char *next_line(void) {
    for (;;) {
        if (g_pos >= g_line_count) return NULL;
        char *l = g_lines[g_pos++].text;
        if (try_parse_anchor(l)) continue;
        return l;
    }
}

static void read_stdin(void) {
    char buf[MAX_LINE_LEN];
    while (fgets(buf, sizeof buf, stdin)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
        if (g_line_count >= MAX_LINES) die_loc("input too large (limit %d lines)", MAX_LINES);
        strncpy(g_lines[g_line_count].text, buf, MAX_LINE_LEN - 1);
        g_lines[g_line_count].text[MAX_LINE_LEN - 1] = '\0';
        g_line_count++;
    }
}

/* ------------------------------------------------------------------ */
/* symbol tables                                                      */
/* ------------------------------------------------------------------ */

static LocalVar *find_local(const char *name) {
    int i;
    for (i = 0; i < g_local_count; i++) if (strcmp(g_locals[i].name, name) == 0) return &g_locals[i];
    return NULL;
}
static GlobalVar *find_global(const char *name) {
    int i;
    for (i = 0; i < g_global_count; i++) if (strcmp(g_globals[i].name, name) == 0) return &g_globals[i];
    return NULL;
}
static int find_struct_index(const char *name) {
    int i;
    for (i = 0; i < g_struct_count; i++) if (strcmp(g_structs[i].name, name) == 0) return i;
    return -1;
}

/* Allocates a stack slot for a local/param, packing tightly but aligned
 * to its own size (same scheme as STRUCT field layout, spec 2.1).
 * struct_id is -1 for a plain u64/u32/ptr, or the index into g_structs
 * if this local was declared with a STRUCT name as its type (physically
 * still just an 8-byte pointer slot - the tag only exists so `var->field`
 * knows which struct's field table to resolve against). */
static int alloc_local(const char *name, Type ty, int struct_id) {
    if (find_local(name)) die_loc("redeclaration of local variable '%s'", name);
    int sz = type_size(ty);
    int off = g_frame_size;
    if (off % sz != 0) off += sz - (off % sz);
    off += sz;
    g_frame_size = off;
    if (g_local_count >= MAX_LOCALS) die_loc("too many locals/parameters in this PROC (limit %d)", MAX_LOCALS);
    LocalVar *v = &g_locals[g_local_count++];
    strncpy(v->name, name, sizeof(v->name) - 1); v->name[sizeof(v->name) - 1] = '\0';
    v->type = ty;
    v->offset = off;
    v->struct_id = struct_id;
    return off;
}

/* ------------------------------------------------------------------ */
/* operand resolution + codegen primitives                            */
/* ------------------------------------------------------------------ */

/* Strips a trailing "[N]" off `seg` in place (if present), and writes N
 * to *index_out. N must be a compile-time integer literal - variable
 * indices aren't supported (see spec/LANGUAGE.md 2.2: why not, and what
 * it would take). Returns 1 if a bracket suffix was found and consumed. */
static int split_index_suffix(char *seg, long *index_out) {
    char *br = strchr(seg, '[');
    if (!br) return 0;
    char *end = strchr(br, ']');
    if (!end || end[1] != '\0') die_loc("malformed array index in '%s'", seg);
    *br = '\0';
    char idxbuf[MAX_TOK];
    size_t len = (size_t)(end - br - 1);
    if (len >= sizeof idxbuf) len = sizeof idxbuf - 1;
    memcpy(idxbuf, br + 1, len);
    idxbuf[len] = '\0';
    unsigned long v;
    if (!parse_int_literal(idxbuf, &v))
        die_loc("array index must be a constant integer literal, got '%s' - variable indices aren't supported", idxbuf);
    *index_out = (long)v;
    return 1;
}

static int resolve_operand(const char *tok, Operand *o) {
    unsigned long v;
    if (parse_int_literal(tok, &v)) { o->kind = OPND_IMM; o->imm = v; o->type = TY_U64; return 1; }
    if (is_register_name(tok)) {
        o->kind = OPND_REG;
        strncpy(o->reg, tok, sizeof(o->reg) - 1); o->reg[sizeof(o->reg) - 1] = '\0';
        o->type = TY_U64;
        return 1;
    }
    if (strstr(tok, "->") || strchr(tok, '[')) {
        char buf[MAX_LINE_LEN];
        strncpy(buf, tok, sizeof buf - 1); buf[sizeof buf - 1] = '\0';

        o->hop_count = 0;
        int cur_struct = -1;   /* which STRUCT's field table the *next* "->name" resolves against */
        int done_indexing = 0; /* a [N] hop was taken - its element type isn't tracked, so no further hops */
        int hop_index = 0;
        char *p = buf;

        for (;;) {
            char *next_arrow = strstr(p, "->");
            char seg[MAX_NAME_LEN];
            size_t seglen = next_arrow ? (size_t)(next_arrow - p) : strlen(p);
            if (seglen == 0 || seglen >= sizeof seg) return 0;
            memcpy(seg, p, seglen); seg[seglen] = '\0';

            long idx = 0;
            int has_index = split_index_suffix(seg, &idx);

            if (hop_index == 0) {
                /* base: must already be a declared local (any type - a
                 * bare ptr local can be indexed with [N] without ever
                 * having been declared with a STRUCT type). */
                LocalVar *lv = find_local(seg);
                if (!lv) return 0; /* let the caller report "unknown identifier" */
                snprintf(o->memref, sizeof(o->memref), "[rbp-%d]", lv->offset);
                cur_struct = lv->struct_id;
                o->type = lv->type;
            } else {
                if (done_indexing) die_loc("cannot chain '->' after a '[...]' index in '%s' (its element type isn't tracked)", tok);
                if (cur_struct < 0) die_loc("cannot chain '->%s' in '%s' - the preceding field isn't a STRUCT-typed field", seg, tok);
                StructDef *sd = &g_structs[cur_struct];
                int fi;
                for (fi = 0; fi < sd->field_count; fi++) if (strcmp(sd->fields[fi].name, seg) == 0) break;
                if (fi == sd->field_count) die_loc("STRUCT '%s' has no field '%s'", sd->name, seg);
                if (o->hop_count >= MAX_CHAIN_HOPS) die_loc("field chain too deep in '%s' (limit %d)", tok, MAX_CHAIN_HOPS);
                o->hop_offsets[o->hop_count++] = sd->fields[fi].offset;
                o->type = sd->fields[fi].type;
                cur_struct = sd->fields[fi].struct_id;
            }

            if (has_index) {
                if (done_indexing) die_loc("cannot chain '[...]' after a '[...]' index in '%s'", tok);
                if (o->hop_count >= MAX_CHAIN_HOPS) die_loc("field chain too deep in '%s' (limit %d)", tok, MAX_CHAIN_HOPS);
                o->hop_offsets[o->hop_count++] = (int)(idx * 8); /* [N] always indexes pointer-width elements */
                o->type = TY_PTR;
                cur_struct = -1;
                done_indexing = 1;
            }

            hop_index++;
            if (!next_arrow) break;
            if (done_indexing) die_loc("cannot chain '->' after a '[...]' index in '%s' (its element type isn't tracked)", tok);
            p = next_arrow + 2;
        }

        if (o->hop_count == 0) return 0; /* bare "var" with no "->field" or "[N]" - not our concern */
        o->kind = OPND_FIELD;
        return 1;
    }
    LocalVar *lv = find_local(tok);
    if (lv) {
        o->kind = OPND_MEM;
        snprintf(o->memref, sizeof(o->memref), "[rbp-%d]", lv->offset);
        o->type = lv->type;
        return 1;
    }
    GlobalVar *gv = find_global(tok);
    if (gv) {
        o->kind = OPND_MEM;
        snprintf(o->memref, sizeof(o->memref), "[%s]", tok);
        o->type = gv->type;
        return 1;
    }
    return 0;
}

/* Loads operand `o` into 64-bit register `dst64`, skipping the move if
 * it's already there. u32 memory/immediate values naturally zero-extend
 * the upper 32 bits of the destination on x86-64.
 *
 * OPND_FIELD (var->field, var->a->b, var[N], ...) is a chain of one or
 * more dereferences: the variable's own slot holds a *pointer*, loaded
 * first into a fixed scratch register (r11, never used as a dst64/
 * ARG_REG/binop register elsewhere in this compiler, so this never
 * collides with dst64), then each hop but the last re-dereferences r11
 * through it; the last hop reads into dst64. A single-hop chain (the
 * common case, `var->field`) degenerates to exactly the two instructions
 * this always emitted before chains existed. */
static void emit_load(const char *dst64, Operand *o) {
    switch (o->kind) {
    case OPND_IMM:
        body_emit("    mov %s, %lu\n", dst64, o->imm);
        break;
    case OPND_REG:
        if (strcmp(dst64, o->reg) != 0) body_emit("    mov %s, %s\n", dst64, o->reg);
        break;
    case OPND_MEM:
        if (o->type == TY_U32) body_emit("    mov %s, %s\n", reg32_of(dst64), o->memref);
        else body_emit("    mov %s, %s\n", dst64, o->memref);
        break;
    case OPND_FIELD: {
        body_emit("    mov r11, %s\n", o->memref);
        int hi;
        for (hi = 0; hi < o->hop_count - 1; hi++)
            body_emit("    mov r11, [r11+%d]\n", o->hop_offsets[hi]);
        int last = o->hop_offsets[o->hop_count - 1];
        if (o->type == TY_U32) body_emit("    mov %s, [r11+%d]\n", reg32_of(dst64), last);
        else body_emit("    mov %s, [r11+%d]\n", dst64, last);
        break;
    }
    }
}

/* Stores 64-bit register `src64` into operand `dst` (a variable, a bare
 * register target, or a var->field / var->a->b / var[N] chain). Never
 * called with src64 == "r11" (see emit_load) so the chain-walking
 * scratch load can't clobber the value being stored. */
static void emit_store(const char *src64, Operand *dst) {
    if (dst->kind == OPND_REG) {
        if (strcmp(dst->reg, src64) != 0) body_emit("    mov %s, %s\n", dst->reg, src64);
    } else if (dst->kind == OPND_MEM) {
        if (dst->type == TY_U32) body_emit("    mov %s, %s\n", dst->memref, reg32_of(src64));
        else body_emit("    mov %s, %s\n", dst->memref, src64);
    } else if (dst->kind == OPND_FIELD) {
        body_emit("    mov r11, %s\n", dst->memref);
        int hi;
        for (hi = 0; hi < dst->hop_count - 1; hi++)
            body_emit("    mov r11, [r11+%d]\n", dst->hop_offsets[hi]);
        int last = dst->hop_offsets[dst->hop_count - 1];
        if (dst->type == TY_U32) body_emit("    mov [r11+%d], %s\n", last, reg32_of(src64));
        else body_emit("    mov [r11+%d], %s\n", last, src64);
    } else {
        die_loc("cannot assign to a constant");
    }
}

/* rax <- rax OP b. b is staged through a register first for uniform,
 * always-correct codegen (this is a deterministic simple compiler, not
 * an optimizer - an immediate-operand form of `shl`/`add`/etc would be
 * one instruction shorter, but every operator here already forgoes that
 * for the same reason). Shifts stage their operand into RCX instead of
 * RBX because x86-64 only supports a variable shift count through CL -
 * `shl reg, reg` isn't a real instruction, `shl reg, cl` is. */
static void emit_binop(const char *op, Operand *b) {
    if (strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
        emit_load("rcx", b);
        if (strcmp(op, "<<") == 0) body_emit("    shl rax, cl\n");
        else body_emit("    shr rax, cl\n"); /* logical shift: every KPL type is unsigned */
        return;
    }
    emit_load("rbx", b);
    if (strcmp(op, "+") == 0) body_emit("    add rax, rbx\n");
    else if (strcmp(op, "-") == 0) body_emit("    sub rax, rbx\n");
    else if (strcmp(op, "*") == 0) body_emit("    imul rax, rbx\n");
    else if (strcmp(op, "/") == 0) {
        body_emit("    xor rdx, rdx\n");
        body_emit("    div rbx\n"); /* unsigned division: every KPL type is unsigned */
    }
    else if (strcmp(op, "&") == 0) body_emit("    and rax, rbx\n");
    else if (strcmp(op, "|") == 0) body_emit("    or rax, rbx\n");
    else if (strcmp(op, "^") == 0) body_emit("    xor rax, rbx\n");
    else die_loc("unknown operator '%s'", op);
}

/* rax <- OP a. Deliberately its own function rather than a special case
 * inside emit_binop: unary operators are a different shape (one operand,
 * not two) and this is where the next one goes, not into emit_binop's
 * switch. */
static void emit_unary_op(const char *op, Operand *a) {
    emit_load("rax", a);
    if (strcmp(op, "~") == 0) body_emit("    not rax\n");
    else die_loc("unknown unary operator '%s'", op);
}

/* cmp lhs, rhs ; <inverse-condition> jump_target  (spec 5.2 / 3.4) */
static void emit_condition_and_jump(const char *lhs, const char *op, const char *rhs, const char *target) {
    Operand a, b;
    if (!resolve_operand(lhs, &a)) die_loc("unknown identifier '%s' in condition", lhs);
    if (!resolve_operand(rhs, &b)) die_loc("unknown identifier '%s' in condition", rhs);
    emit_load("rax", &a);
    emit_load("rbx", &b);
    body_emit("    cmp rax, rbx\n");
    const char *inv;
    if (strcmp(op, "==") == 0) inv = "jne";
    else if (strcmp(op, "!=") == 0) inv = "je";
    else if (strcmp(op, "<") == 0) inv = "jae"; /* unsigned: skip if NOT less */
    else if (strcmp(op, ">") == 0) inv = "jbe"; /* unsigned: skip if NOT greater */
    else { die_loc("unknown comparison operator '%s' (expected ==, !=, < or >)", op); return; }
    body_emit("    %s %s\n", inv, target);
}

static void push_ctrl(BlockKind kind, int label_id) {
    if (g_ctrl_top >= MAX_NEST) die_loc("if/loop nesting too deep (limit %d)", MAX_NEST);
    g_ctrl_stack[g_ctrl_top].kind = kind;
    g_ctrl_stack[g_ctrl_top].label_id = label_id;
    g_ctrl_top++;
}

/* ------------------------------------------------------------------ */
/* ASM raw block: mirrors characters verbatim, substituting [localvar]  */
/* references for their [rbp-N] stack address (spec 6.1-6.3)           */
/* ------------------------------------------------------------------ */

static void substitute_var_refs(const char *in, char *out, size_t outsz) {
    size_t on = 0;
    const char *p = in;
    while (*p && on < outsz - 1) {
        if (*p == '[') {
            const char *start = p + 1;
            const char *q = start;
            while (*q && *q != ']') q++;
            if (*q == ']' && q > start && is_ident_start((unsigned char)start[0])) {
                size_t idlen = (size_t)(q - start);
                int pure_ident = (idlen < MAX_NAME_LEN);
                size_t k;
                for (k = 0; pure_ident && k < idlen; k++)
                    if (!is_ident_char((unsigned char)start[k])) pure_ident = 0;
                if (pure_ident) {
                    char id[MAX_NAME_LEN];
                    memcpy(id, start, idlen);
                    id[idlen] = '\0';
                    LocalVar *lv = find_local(id);
                    if (lv) {
                        char rep[64];
                        int rl = snprintf(rep, sizeof rep, "[rbp-%d]", lv->offset);
                        size_t k2;
                        for (k2 = 0; k2 < (size_t)rl && on < outsz - 1; k2++) out[on++] = rep[k2];
                        p = q + 1;
                        continue;
                    }
                }
            }
        }
        out[on++] = *p++;
    }
    out[on] = '\0';
}

/* Consumes one line-fragment of raw ASM text, tracking brace depth
 * (ignoring anything after a ';' comment) and mirroring everything up
 * to - but excluding - the closing brace that brings depth to zero. */
static void process_asm_fragment(const char *text, int *depth) {
    char raw[MAX_LINE_LEN];
    size_t rn = 0;
    int in_comment = 0;
    const char *p = text;
    for (; *p && rn < sizeof(raw) - 1; p++) {
        if (!in_comment && *p == ';') in_comment = 1;
        if (!in_comment) {
            if (*p == '{') { (*depth)++; }
            else if (*p == '}') {
                (*depth)--;
                if (*depth <= 0) break; /* exclude the delimiter itself */
            }
        }
        raw[rn++] = *p;
    }
    raw[rn] = '\0';
    char sub[MAX_LINE_LEN * 2];
    substitute_var_refs(raw, sub, sizeof sub);
    body_emit("%s\n", sub);
}

static void handle_asm(char *line) {
    char *after = trim(skip_ws(line + 3)); /* past "ASM" */
    if (after[0] != '{') die_loc("expected '{' after ASM");
    int depth = 1;
    process_asm_fragment(after + 1, &depth);
    while (depth > 0) {
        char *raw = next_line();
        if (!raw) die_loc("unterminated ASM block");
        process_asm_fragment(raw, &depth);
    }
}

/* ------------------------------------------------------------------ */
/* if / loop                                                          */
/* ------------------------------------------------------------------ */

static void parse_condition(char *cond, char toks[3][MAX_TOK], const char *stmt_kind) {
    size_t clen = strlen(cond);
    if (clen == 0 || cond[clen - 1] != '{') die_loc("expected '{' at end of '%s' statement", stmt_kind);
    cond[clen - 1] = '\0';
    cond = trim(cond);
    int n = split_ws(cond, toks, 3);
    if (n != 3 || !(strcmp(toks[1], "==") == 0 || strcmp(toks[1], "!=") == 0 || strcmp(toks[1], "<") == 0 || strcmp(toks[1], ">") == 0))
        die_loc("'%s' condition must be exactly 'lhs OP rhs' with OP one of ==, !=, <, >", stmt_kind);
}

static void handle_if(char *line) {
    char *cond = trim(line + 2);
    char toks[3][MAX_TOK];
    parse_condition(cond, toks, "if");
    int id = g_label_id++;
    char lbl[32];
    snprintf(lbl, sizeof lbl, ".L_ELSE_%d", id);
    emit_condition_and_jump(toks[0], toks[1], toks[2], lbl);
    push_ctrl(BLK_IF, id);
}

static void handle_loop(char *line) {
    char *cond = trim(line + 4);
    char toks[3][MAX_TOK];
    parse_condition(cond, toks, "loop");
    int id = g_label_id++;
    body_emit(".L_LOOP_%d:\n", id);
    char lbl[32];
    snprintf(lbl, sizeof lbl, ".L_LOOPEND_%d", id);
    emit_condition_and_jump(toks[0], toks[1], toks[2], lbl);
    push_ctrl(BLK_LOOP, id);
}

/* ------------------------------------------------------------------ */
/* declarations / assignments / calls                                 */
/* ------------------------------------------------------------------ */

/* True if `rhs` is exactly `identifier(args)` with nothing else around
 * it - i.e. a call used as an expression, not a call statement. */
static int looks_like_call(const char *rhs) {
    const char *op = strchr(rhs, '(');
    if (!op || op == rhs) return 0;
    size_t len = strlen(rhs);
    if (len == 0 || rhs[len - 1] != ')') return 0;
    const char *p = rhs;
    for (; p < op; p++) if (!is_ident_char((unsigned char)*p)) return 0;
    return 1;
}

/* Emits a call to `line`, which must look like `identifier(args)` -
 * loading up to 6 arguments into their ABI registers and calling it.
 * Used both as a full statement (result discarded) and as the RHS of a
 * declaration/assignment/RETURN (result read from rax by the caller,
 * per the System V ABI - the same convention every call this compiler
 * emits already relies on for its own PROC prologues/epilogues). */
static void emit_call(char *line) {
    char *op = strchr(line, '(');
    char *cp = strrchr(line, ')');
    if (!op || !cp || cp < op) die_loc("malformed call: %s", line);

    char name[MAX_NAME_LEN];
    size_t nlen = (size_t)(op - line);
    if (nlen >= sizeof name) nlen = sizeof(name) - 1;
    memcpy(name, line, nlen); name[nlen] = '\0';
    char *tname = trim(name);

    char argstr[MAX_LINE_LEN];
    size_t alen = (size_t)(cp - op - 1);
    if (alen >= sizeof argstr) alen = sizeof(argstr) - 1;
    memcpy(argstr, op + 1, alen); argstr[alen] = '\0';

    char *targs = trim(argstr);
    char toks[8][MAX_TOK];
    int n = (targs[0] == '\0') ? 0 : split_commas(targs, toks, 8);
    if (n > 6) die_loc("call to '%s' passes %d arguments, maximum is 6 (System V ABI)", tname, n);

    static const char *ARG_REGS[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    int i;
    for (i = 0; i < n; i++) {
        char *a = trim(toks[i]);
        Operand o;
        if (!resolve_operand(a, &o)) die_loc("unknown identifier '%s' in call to '%s'", a, tname);
        emit_load(ARG_REGS[i], &o);
    }
    record_called_name(tname);
    body_emit("    call %s\n", tname);
}

/* Computes a linear expression (a single operand, `a OP b`, or a call
 * `name(args)`) into rax. Shared by declarations/assignments (which then
 * store rax into a destination) and RETURN (which leaves it in rax for
 * the epilogue - the System V ABI's own return-value register, so no
 * extra move is needed). A call-expression is treated as one flat
 * operand, the same tier as a bare variable name - `u64 x = f() + 1` is
 * not supported any more than `u64 x = a + b * c` is; split it into two
 * lines, same as any other linearity violation. */
static void emit_rhs_to_rax(char *rhs) {
    if (looks_like_call(rhs)) {
        emit_call(rhs);
        return;
    }
    char toks[3][MAX_TOK];
    int n = split_ws(rhs, toks, 3);
    if (n == 1) {
        if (toks[0][0] == '~' && toks[0][1] != '\0')
            die_loc("unary operator '~' needs a space before its operand: '~ %s'", toks[0] + 1);
        Operand src;
        if (!resolve_operand(toks[0], &src)) die_loc("unknown identifier '%s'", toks[0]);
        emit_load("rax", &src);
    } else if (n == 2 && is_unary_op_token(toks[0])) {
        Operand a;
        if (!resolve_operand(toks[1], &a)) die_loc("unknown identifier '%s'", toks[1]);
        emit_unary_op(toks[0], &a);
    } else if (n == 3 && is_op_token(toks[1])) {
        Operand a, b;
        if (!resolve_operand(toks[0], &a)) die_loc("unknown identifier '%s'", toks[0]);
        if (!resolve_operand(toks[2], &b)) die_loc("unknown identifier '%s'", toks[2]);
        emit_load("rax", &a);
        emit_binop(toks[1], &b);
    } else {
        die_loc("expression violates the linearity constraint (max one operator per line): '%s'", rhs);
    }
}

static void emit_rhs_into(Operand *dst, char *rhs) {
    emit_rhs_to_rax(rhs);
    emit_store("rax", dst);
}

static void handle_decl_or_assign(char *line) {
    char *eq = strchr(line, '=');
    if (!eq) die_loc("expected '=' in statement: %s", line);
    *eq = '\0';
    char *lhs = trim(line);
    char *rhs = trim(eq + 1);

    char toks[3][MAX_TOK];
    int n = split_ws(lhs, toks, 3);

    Operand dst;
    if (n == 2) {
        Type ty;
        int struct_id = -1;
        if (!type_from_str(toks[0], &ty)) {
            struct_id = find_struct_index(toks[0]);
            if (struct_id < 0) die_loc("unknown type '%s'", toks[0]);
            ty = TY_PTR; /* a "StructName var" local is physically just a pointer */
        }
        int off = alloc_local(toks[1], ty, struct_id);
        dst.kind = OPND_MEM;
        snprintf(dst.memref, sizeof dst.memref, "[rbp-%d]", off);
        dst.type = ty;
    } else if (n == 1) {
        if (!resolve_operand(toks[0], &dst) || dst.kind == OPND_IMM)
            die_loc("assignment to unknown identifier '%s'", toks[0]);
    } else {
        die_loc("malformed declaration/assignment: %s", lhs);
    }

    emit_rhs_into(&dst, rhs);
}

/* ------------------------------------------------------------------ */
/* PROC body statement dispatch                                       */
/* ------------------------------------------------------------------ */

#define PROC_CONTINUE 0
#define PROC_END 1

static int process_proc_line(char *raw) {
    char code[MAX_LINE_LEN], comment[MAX_LINE_LEN];
    split_comment(raw, code, sizeof code, comment, sizeof comment);
    char *line = code;

    if (*line == '\0') {
        if (comment[0]) body_emit("    ; %s\n", comment);
        return PROC_CONTINUE;
    }

    while (line[0] == '}') {
        line = trim(line + 1);
        if (g_ctrl_top == 0) {
            if (*line != '\0') die_loc("unexpected tokens after unmatched '}': %s", line);
            if (comment[0]) body_emit("    ; %s\n", comment);
            return PROC_END;
        }
        CtrlBlock *top = &g_ctrl_stack[g_ctrl_top - 1];
        if (top->kind == BLK_LOOP) {
            body_emit("    jmp .L_LOOP_%d\n", top->label_id);
            body_emit(".L_LOOPEND_%d:\n", top->label_id);
            g_ctrl_top--;
            continue;
        }
        if (top->kind == BLK_IF && starts_with_word(line, "else")) {
            char *after = trim(skip_ws(line + 4));
            if (after[0] != '{') die_loc("expected '{' after 'else'");
            int id = top->label_id;
            body_emit("    jmp .L_END_%d\n", id);
            body_emit(".L_ELSE_%d:\n", id);
            g_ctrl_top--;
            push_ctrl(BLK_ELSE, id);
            line = trim(after + 1);
            continue;
        }
        if (top->kind == BLK_IF) body_emit(".L_ELSE_%d:\n", top->label_id);
        else body_emit(".L_END_%d:\n", top->label_id);
        g_ctrl_top--;
    }

    if (*line == '\0') {
        if (comment[0]) body_emit("    ; %s\n", comment);
        return PROC_CONTINUE;
    }

    if (starts_with_word(line, "if")) handle_if(line);
    else if (starts_with_word(line, "loop")) handle_loop(line);
    else if (starts_with_word(line, "ASM")) handle_asm(line);
    else if (starts_with_word(line, "RETURN")) {
        char *expr = trim(line + 6);
        if (*expr != '\0') emit_rhs_to_rax(expr);
        body_emit("    mov rsp, rbp\n");
        body_emit("    pop rbp\n");
        body_emit("    ret\n");
    }
    else if (strchr(line, '=')) handle_decl_or_assign(line);
    else if (strchr(line, '(')) emit_call(line);
    else die_loc("unrecognized statement: %s", line);

    if (comment[0]) body_emit("    ; %s\n", comment);
    return PROC_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* top-level: STRUCT / PROC / global variable declarations            */
/* ------------------------------------------------------------------ */

static void parse_name_after_keyword(char *line, const char *keyword, char *out, size_t outsz) {
    char *p = skip_ws(line);
    p += strlen(keyword);
    p = skip_ws(p);
    size_t i = 0;
    while (is_ident_char((unsigned char)*p) && i < outsz - 1) out[i++] = *p++;
    out[i] = '\0';
}

static void handle_struct(char *line) {
    char name[MAX_NAME_LEN];
    parse_name_after_keyword(line, "STRUCT", name, sizeof name);
    if (g_struct_count >= MAX_STRUCTS) die_loc("too many STRUCT definitions (limit %d)", MAX_STRUCTS);
    StructDef *sd = &g_structs[g_struct_count++];
    strncpy(sd->name, name, sizeof(sd->name) - 1); sd->name[sizeof(sd->name) - 1] = '\0';
    sd->field_count = 0;
    int offset = 0;

    char *raw;
    while ((raw = next_line()) != NULL) {
        char code[MAX_LINE_LEN], comment[MAX_LINE_LEN];
        split_comment(raw, code, sizeof code, comment, sizeof comment);
        char *l = code;
        if (*l == '\0') continue;
        if (l[0] == '}') { sd->size = offset; return; }

        char toks[2][MAX_TOK];
        int n = split_ws(l, toks, 2);
        if (n != 2) die_loc("malformed STRUCT field (expected 'type name'): %s", l);
        Type ty;
        int fld_struct_id = -1;
        if (!type_from_str(toks[0], &ty)) {
            fld_struct_id = find_struct_index(toks[0]);
            if (fld_struct_id < 0) die_loc("unknown type '%s' in STRUCT field", toks[0]);
            ty = TY_PTR; /* a "StructName field" is physically just a pointer, same as locals */
        }
        int sz = type_size(ty);
        if (offset % sz != 0) offset += sz - (offset % sz);
        if (sd->field_count >= MAX_FIELDS) die_loc("too many fields in STRUCT %s (limit %d)", sd->name, MAX_FIELDS);
        StructField *f = &sd->fields[sd->field_count++];
        strncpy(f->name, toks[1], sizeof(f->name) - 1); f->name[sizeof(f->name) - 1] = '\0';
        f->type = ty;
        f->offset = offset;
        f->struct_id = fld_struct_id;
        offset += sz;
    }
    die_loc("unterminated STRUCT '%s'", name);
}

static void handle_proc(char *line) {
    char name[MAX_NAME_LEN];
    parse_name_after_keyword(line, "PROC", name, sizeof name);
    record_defined_proc(name);

    char anchor_file[MAX_PATH_LEN];
    strncpy(anchor_file, g_cur_file, sizeof anchor_file - 1); anchor_file[sizeof anchor_file - 1] = '\0';
    int anchor_line = g_cur_src_line;

    char *op = strchr(line, '(');
    char *cp = strchr(line, ')');
    if (!op || !cp || cp < op) die_loc("malformed PROC parameter list for '%s'", name);
    if (!strchr(cp, '{')) die_loc("expected '{' after PROC '%s' parameter list", name);

    char paramstr[MAX_LINE_LEN];
    size_t plen = (size_t)(cp - op - 1);
    if (plen >= sizeof paramstr) plen = sizeof(paramstr) - 1;
    memcpy(paramstr, op + 1, plen); paramstr[plen] = '\0';

    g_local_count = 0;
    g_frame_size = 0;
    g_ctrl_top = 0;
    g_body_len = 0;
    g_body_buf[0] = '\0';

    static const char *ARG_REGS[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    char *tparams = trim(paramstr);
    if (tparams[0] != '\0') {
        char ptoks[8][MAX_TOK];
        int pn = split_commas(tparams, ptoks, 8);
        if (pn > 6) die_loc("PROC '%s' has %d parameters, maximum is 6 (System V ABI)", name, pn);
        int i;
        for (i = 0; i < pn; i++) {
            char *tok = trim(ptoks[i]);
            char *colon = strchr(tok, ':');
            if (!colon) die_loc("malformed parameter '%s' in PROC '%s' (expected 'name: type')", tok, name);
            *colon = '\0';
            char *pname = trim(tok);
            char *ptype = trim(colon + 1);
            Type ty;
            if (!type_from_str(ptype, &ty)) die_loc("unknown parameter type '%s'", ptype);
            int off = alloc_local(pname, ty, -1);
            if (ty == TY_U32) body_emit("    mov [rbp-%d], %s\n", off, reg32_of(ARG_REGS[i]));
            else body_emit("    mov [rbp-%d], %s\n", off, ARG_REGS[i]);
        }
    }

    for (;;) {
        char *raw = next_line();
        if (!raw) die_loc("unterminated PROC '%s'", name);
        if (process_proc_line(raw) == PROC_END) break;
    }
    if (g_ctrl_top != 0) die_loc("PROC '%s' ended with unclosed if/loop block(s)", name);

    int frame = g_frame_size;
    if (frame % 16 != 0) frame += 16 - (frame % 16); /* 16-byte aligned, spec 4.3 */

    text_emit("; --- %s:%d %s ---\n", anchor_file, anchor_line, name);
    text_emit("global %s\n", name);
    text_emit("%s:\n", name);
    text_emit("    push rbp\n");
    text_emit("    mov rbp, rsp\n");
    if (frame > 0) text_emit("    sub rsp, %d\n", frame);
    text_emit("%s", g_body_buf);
    text_emit("    mov rsp, rbp\n");
    text_emit("    pop rbp\n");
    text_emit("    ret\n\n");
}

static void handle_global_decl(char *line, const char *comment) {
    char *eq = strchr(line, '=');
    if (!eq) die_loc("expected 'type name = value' at global scope: %s", line);
    *eq = '\0';
    char *lhs = trim(line);
    char *rhs = trim(eq + 1);

    char toks[2][MAX_TOK];
    int n = split_ws(lhs, toks, 2);
    if (n != 2) die_loc("malformed global declaration: %s", line);
    Type ty;
    if (!type_from_str(toks[0], &ty)) die_loc("unknown type '%s'", toks[0]);
    const char *name = toks[1];

    unsigned long val;
    if (!parse_int_literal(rhs, &val)) die_loc("global initializer must be a constant integer literal, got '%s'", rhs);

    if (find_global(name)) die_loc("redeclaration of global variable '%s'", name);
    if (g_global_count >= MAX_GLOBALS) die_loc("too many global variables (limit %d)", MAX_GLOBALS);
    GlobalVar *gv = &g_globals[g_global_count++];
    strncpy(gv->name, name, sizeof(gv->name) - 1); gv->name[sizeof(gv->name) - 1] = '\0';
    gv->type = ty;

    const char *directive = (ty == TY_U32) ? "dd" : "dq";
    if (comment[0]) data_emit("%s: %s %lu  ; %s\n", name, directive, val, comment);
    else data_emit("%s: %s %lu\n", name, directive, val);
}

static void process_top_level(void) {
    char *raw;
    while ((raw = next_line()) != NULL) {
        char code[MAX_LINE_LEN], comment[MAX_LINE_LEN];
        split_comment(raw, code, sizeof code, comment, sizeof comment);
        char *line = code;

        if (*line == '\0') {
            if (comment[0]) text_emit("; %s\n", comment);
            continue;
        }
        if (starts_with_word(line, "STRUCT")) { handle_struct(line); continue; }
        if (starts_with_word(line, "PROC")) { handle_proc(line); continue; }
        handle_global_decl(line, comment);
    }
}

/* ------------------------------------------------------------------ */
/* `kpl init` scaffolding (spec/COMPILER.md section 4)                */
/* ------------------------------------------------------------------ */

static int write_file_if_absent(const char *path, const char *content) {
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); fprintf(stderr, "kpl: init: '%s' already exists, skipping\n", path); return 0; }
    f = fopen(path, "w");
    if (!f) { fprintf(stderr, "kpl: init: cannot create '%s'\n", path); return 1; }
    fputs(content, f);
    fclose(f);
    printf("kpl: init: created %s\n", path);
    return 0;
}

static int cmd_init(void) {
    int rc = 0;
    rc |= write_file_if_absent("linker.ld",
        "OUTPUT_FORMAT(elf64-x86-64)\n"
        "ENTRY(kpl_main)\n"
        "SECTIONS {\n"
        "    . = 0xFFFFFFFF80000000; /* maps directly to Limine higher-half rules */\n"
        "    .text :   { *(.text*) }\n"
        "    .rodata : { *(.rodata*) }\n"
        "    .data :   { *(.data*) }\n"
        "    .bss :    { *(.bss*) *(COMMON) }\n"
        "}\n");
    rc |= write_file_if_absent("Makefile",
        "all: kpl_kernel.bin\n"
        "\n"
        "kpl_kernel.bin: src/main.kpl\n"
        "\t@mkdir -p build\n"
        "\tkpp -I. src/main.kpl | kpl -o build/kernel.asm\n"
        "\tnasm -f elf64 build/kernel.asm -o build/kernel.o\n"
        "\tld -nostdlib -z max-page-size=0x1000 -T linker.ld build/kernel.o -o kpl_kernel.bin\n"
        "\n"
        "clean:\n"
        "\trm -rf build kpl_kernel.bin\n");
    if (system("mkdir -p src") != 0) { fprintf(stderr, "kpl: init: failed to create 'src' directory\n"); return 1; }
    rc |= write_file_if_absent("src/main.kpl",
        "; Deployed via kpl init\n"
        "PROC kpl_main() {\n"
        "    ASM {\n"
        "        cli\n"
        "    .loop:\n"
        "        hlt\n"
        "        jmp .loop\n"
        "    }\n"
        "}\n");
    return rc;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "init") == 0) return cmd_init();

    const char *outpath = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outpath = argv[++i];
        else { fprintf(stderr, "kpl: error: unrecognized argument '%s'\n", argv[i]); return 1; }
    }

    read_stdin();
    process_top_level();

    FILE *out = outpath ? fopen(outpath, "w") : stdout;
    if (!out) { fprintf(stderr, "kpl: error: cannot open output file '%s'\n", outpath); return 1; }

    int j, wrote_extern = 0;
    for (j = 0; j < g_called_name_count; j++) {
        if (!name_in_table(g_defined_procs, g_defined_proc_count, g_called_names[j])) {
            fprintf(out, "extern %s\n", g_called_names[j]);
            wrote_extern = 1;
        }
    }
    if (wrote_extern) fprintf(out, "\n");

    if (g_text_len > 0) { fprintf(out, "section .text\n\n"); fwrite(g_text_buf, 1, g_text_len, out); }
    if (g_data_len > 0) { fprintf(out, "\nsection .data\n\n"); fwrite(g_data_buf, 1, g_data_len, out); }

    if (outpath) fclose(out);
    return 0;
}

/* gsos.c -- the builtins that read files or the environment, through GS/OS
 * (docs/GSOS.md, DECISIONS.md D30): pathExists, readFile, readFileType,
 * readDir, hashFile and getEnv. Each is Nix 2.34's (primops.cc) rule for
 * rule; where GS/OS is not there to answer (GSOS_ADDR 0) each is E_GSOS by
 * name, as before (D26).
 *
 * A Nix path is passed to GS/OS as its full pathname, unchanged: `/` is a
 * GS/OS separator. GS/OS has no symlinks; ours are files of type $F1, aux
 * type "NSL", holding their target (D30), and the VM resolves them itself as
 * Nix's SourceAccessor::resolveSymlinks does. The path being walked, what is
 * left of it and a link's target live in bank 7, the scratch bank (VM.md):
 * none of this runs bytecode once the argument is forced.
 */
#pragma noroot
#pragma optimize 15

#include "prims.h"

segment "gsos";

#define G_RES   ((uint8_t *)0x00070000UL)   /* GSString: the path so far, what GS/OS is given */
#define G_REST  ((uint8_t *)0x00071000UL)   /* the components still to walk */
#define G_TMP   ((uint8_t *)0x00072000UL)   /* a link's target; a ResultBuf for a name or a value */
#define G_SPLICE ((uint8_t *)0x00073000UL)  /* a target spliced before the rest */
#define G_MAX   0x0F00U                     /* a path's longest, in this walk */

/* What GS/OS calls a file (GSOS.md "Symlinks") */
#define FT_DIR      0x0F
#define FT_LINK     0xF1
#define AUX_LINK    0x004C534EUL            /* "NSL" */
#define K_FILE      1
#define K_DIR       2
#define K_LINK      3

#define ERR_PATH    0x44                    /* pathNotFound */
#define ERR_VOL     0x45                    /* volNotFound */
#define ERR_FILE    0x46                    /* fileNotFound */
#define ERR_ENDDIR  0x61                    /* endOfDir */

#define R_NONE      0                       /* Nix's SymlinkResolution: none -- and then no
                                               ancestor may be a link (SymlinkNotAllowed) --, */
#define R_ANCESTORS 1                       /* the ancestors only, */
#define R_FULL      2                       /* or every component */

struct gs_fileinfo { uint16_t pcount; void *path; uint16_t access, ftype; uint32_t aux; uint16_t stype; };
struct gs_open     { uint16_t pcount, ref; void *path; uint16_t access; };
struct gs_io       { uint16_t pcount, ref; void *buf; uint32_t req, got; };
struct gs_eof      { uint16_t pcount, ref; uint32_t eof; };
struct gs_dirent   { uint16_t pcount, ref, flags, base, disp; void *name; uint16_t entry, ftype;
                  uint32_t eof, blocks; uint8_t created[8], modified[8]; uint16_t access; uint32_t aux; };
struct gs_readvar  { uint16_t pcount; void *name, *value; };

/* GS/OS's stack entry: the parameter block's address and the call number
 * pushed, then JSL $E100B0, which pops them and returns the error in A */
static uint16_t gs_call(uint16_t num, void *pb)
{
    uint16_t err;
    asm {
        lda pb+2
        pha
        lda pb
        pha
        lda num
        pha
        jsl 0xE100B0
        sta err
    }
    return err;
}

static uint8_t kind_of(uint16_t ftype, uint32_t aux)
{
    if (ftype == FT_DIR) return K_DIR;
    if (ftype == FT_LINK && aux == AUX_LINK) return K_LINK;
    return K_FILE;
}

/* GetFileInfo of the path in G_RES: its kind, or 0 with *err the GS/OS error */
static uint8_t res_kind(uint16_t *err)
{
    struct gs_fileinfo fi;
    fi.pcount = 5;
    fi.path = G_RES;
    *err = gs_call(0x2006, &fi);
    if (*err) return 0;
    if (fi.stype == 0x0D || fi.stype == 0x0F) return K_DIR;   /* a directory, a volume */
    return kind_of(fi.ftype, fi.aux);
}

/* E_FILE unless GS/OS answered */
static void check(uint16_t err)
{
    if (err) err_exit(E_FILE);
}

static uint16_t res_open(void)
{
    struct gs_open o;
    o.pcount = 3;
    o.path = G_RES;
    o.access = 1;                               /* read */
    check(gs_call(0x2010, &o));
    return o.ref;
}

static void res_close(uint16_t ref)
{
    struct gs_eof c;                               /* Close takes the refNum alone */
    c.pcount = 1;
    c.ref = ref;
    (void)gs_call(0x2014, &c);
}

static uint32_t ref_eof(uint16_t ref)
{
    struct gs_eof e;
    e.pcount = 2;
    e.ref = ref;
    check(gs_call(0x2019, &e));
    return e.eof;
}

static void ref_read(uint16_t ref, void *dst, uint16_t n)
{
    struct gs_io r;
    if (!n) return;
    r.pcount = 4;
    r.ref = ref;
    r.buf = (void *)((uint32_t)dst & 0x00FFFFFFUL);   /* a young pointer's tag is not an address */
    r.req = n;
    check(gs_call(0x2012, &r));
    if (r.got != n) err_exit(E_FILE);
}

/* The path in G_RES ends one component shorter (the root stays the root) */
static void res_pop(void)
{
    uint16_t n = *(uint16_t *)G_RES;
    while (n && G_RES[2 + n - 1] != '/') --n;
    if (n) --n;                                 /* the '/' before it */
    *(uint16_t *)G_RES = n;
}

/* Walk the canonical absolute path at G_REST[0..rn) into G_RES as Nix's
 * resolveSymlinks does (GSOS.md "Symlinks"): a component at a time, `.`
 * skipped, `..` popped, every other pushed and, if it is a link -- always in
 * R_FULL, unless it is the last in R_ANCESTORS -- replaced by its target's
 * components, from the root for an absolute target. 1,024 links. In R_NONE
 * nothing is replaced, and a link before the last component is E_FILE, as
 * Nix's PosixSourceAccessor asserts of every path it touches. */
static void walk(uint16_t rn, uint8_t mode)
{
    uint16_t off = 0, links = 1024, c, cl, i, n, tn, ref, err;
    *(uint16_t *)G_RES = 0;
    while (off < rn) {
        while (off < rn && G_REST[off] == '/') ++off;
        if (off == rn) break;
        for (cl = 0; off + cl < rn && G_REST[off + cl] != '/'; ++cl) ;
        c = off;
        off += cl;
        if (cl == 1 && G_REST[c] == '.') continue;
        if (cl == 2 && G_REST[c] == '.' && G_REST[c + 1] == '.') { res_pop(); continue; }
        n = *(uint16_t *)G_RES;
        if (n + 1 + cl > G_MAX) nomem();
        G_RES[2 + n] = '/';
        for (i = 0; i < cl; ++i) {
            if (G_REST[c + i] == ':') gsos_exit(":", 1);      /* no FST has a ':' in a name */
            G_RES[3 + n + i] = G_REST[c + i];
        }
        *(uint16_t *)G_RES = n + 1 + cl;
        if (mode != R_FULL) {                   /* the last component stays as it is */
            for (i = off; i < rn && G_REST[i] == '/'; ++i) ;
            if (i == rn) continue;
        }
        if (res_kind(&err) != K_LINK) continue;
        if (mode == R_NONE) err_exit(E_FILE);   /* "path '...' is a symlink" */
        if (links-- == 0) err_exit(E_FILE);     /* "infinite symlink recursion" */
        ref = res_open();
        tn = (uint16_t)ref_eof(ref);
        if (tn > G_MAX || tn + 1 + (rn - off) > G_MAX) nomem();
        ref_read(ref, G_TMP, tn);
        res_close(ref);
        if (tn && G_TMP[0] == '/') *(uint16_t *)G_RES = 0;
        else res_pop();
        memcpy(G_SPLICE, G_TMP, tn);            /* the target, then the rest */
        G_SPLICE[tn] = '/';
        memcpy(G_SPLICE + tn + 1, G_REST + off, rn - off);
        rn = tn + 1 + (rn - off);
        memcpy(G_REST, G_SPLICE, rn);
        off = 0;
    }
    if (*(uint16_t *)G_RES == 0) gsos_exit("/", 1);   /* the root has no GS/OS name */
}

/* The forced argument on top as Nix's coerceToPath has it -- a path its text,
 * a set through __toString or outPath, a string that must be absolute
 * (E_TYPE) and is made canonical -- into G_REST; its length. A string with a
 * context names store paths to build first: E_GSOS. *dir: a string that
 * ends in "/" or "/." (pathExists's mustBeDir). Pops it. */
static uint16_t path_arg(uint8_t *dir)
{
    obj_t *s, *p;
    uint16_t n;
    s = deref(peek());
    n = s->tag == T_STRING ? AUX16(s) : 0;
    *dir = n && (STRBYTES(s)[n - 1] == '/' || (n >= 2 && STRBYTES(s)[n - 1] == '.' && STRBYTES(s)[n - 2] == '/'));
    coerce_top(CO_PATH);
    s = deref(peek());
    n = AUX16(s);
    if (n == 0 || STRBYTES(s)[0] != '/') type_err();   /* "doesn't represent an absolute path" */
    if (s->tag == T_STRING && CTX(s)) gsos_exit("realise", 7);
    if (n >= G_MAX) nomem();
    nursery_reserve(round4(n + 1) + 12);
    s = deref(peek());
    p = path_join(STRBYTES(s), n, "", 0);       /* Nix's canonPath */
    n = AUX16(p);
    memcpy(G_REST, STRBYTES(p), n);
    --vsp;
    return n;
}

static int gsos_here(void)
{
    return *(volatile uint8_t *)GSOS_ADDR != 0;
}

/* A fresh STRING of the C literal t[0..n) */
static obj_t *new_lit(const char *t, uint16_t n)
{
    obj_t *r;
    nursery_reserve(round4(n) + 8);
    r = str_alloc(n);
    memcpy(STRBYTES(r), t, n);
    return r;
}

/* The file at G_RES, read whole into a fresh STRING pushed on the stack.
 * For readFile a NUL in it is E_FILE ("cannot be represented as a Nix
 * string"); hashFile hashes the bytes, NULs and all. */
static void push_file(uint8_t text)
{
    uint16_t ref, n, i, err;
    uint32_t len;
    obj_t *r;
    char *b;
    if (res_kind(&err) != K_FILE) err_exit(E_FILE);   /* not there, or a directory: GS/OS opens those */
    ref = res_open();
    len = ref_eof(ref);
    if (len > 0xFFF0UL) nomem();                /* a STRING is at most a bank */
    n = (uint16_t)len;
    nursery_reserve(round4(n) + 8);             /* holds nothing: the refNum is a number */
    r = str_alloc(n);
    b = STRBYTES(r);
    ref_read(ref, b, n);
    res_close(ref);
    if (text) for (i = 0; i < n; ++i) if (b[i] == 0) err_exit(E_FILE);
    push(r);
}

/* --- the builtins ---------------------------------------------------------------------- */

void prim_pathexists(void)                      /* pathExists p */
{
    uint8_t dir, k;
    uint16_t n, err;
    if (!gsos_here()) prim_gsos(PRIM_PATHEXISTS, 1);
    force_top();
    n = path_arg(&dir);
    walk(n, dir ? R_FULL : R_ANCESTORS);
    k = res_kind(&err);
    if (!k && err != ERR_PATH && err != ERR_VOL && err != ERR_FILE) err_exit(E_FILE);
    push(k && (!dir || k == K_DIR) ? obj_true : obj_false);
}

void prim_readfile(void)                        /* readFile p */
{
    uint8_t dir;
    if (!gsos_here()) prim_gsos(PRIM_READFILE, 1);
    force_top();
    walk(path_arg(&dir), R_FULL);
    push_file(1);
}

void prim_readfiletype(void)                    /* readFileType p: lstat's type, nothing resolved */
{
    uint8_t dir, k;
    uint16_t err;
    if (!gsos_here()) prim_gsos(PRIM_READFILETYPE, 1);
    force_top();
    walk(path_arg(&dir), R_NONE);
    k = res_kind(&err);
    check(err);
    if (k == K_DIR) push(new_lit("directory", 9));
    else if (k == K_LINK) push(new_lit("symlink", 7));
    else push(new_lit("regular", 7));
}

void prim_readdir(void)                         /* readDir p: { name = type; ... } */
{
    uint8_t dir, k;
    uint16_t ref, err, n = 0, b, i, cnt = 0, len;
    struct gs_dirent d;
    obj_t *set, *nm;
    if (!gsos_here()) prim_gsos(PRIM_READDIR, 1);
    force_top();
    walk(path_arg(&dir), R_FULL);
    b = vsp;                                    /* the argument is gone: the pairs start here */
    ref = res_open();                           /* not a directory: GetDirEntry's error, E_FILE */
    for (;;) {                                  /* each entry: its name, then its type */
        *(uint16_t *)G_TMP = G_MAX;             /* a ResultBuf: its size, then a GSString */
        d.pcount = 13;
        d.ref = ref;
        d.base = 1;                             /* the next entry */
        d.disp = 1;
        d.name = G_TMP;
        err = gs_call(0x201C, &d);
        if (err == ERR_ENDDIR) break;
        if (err) { res_close(ref); err_exit(E_FILE); }
        len = *(uint16_t *)(G_TMP + 2);
        nursery_reserve(round4(len) + 8);
        nm = str_alloc(len);
        memcpy(STRBYTES(nm), G_TMP + 4, len);
        push(nm);
        k = kind_of(d.ftype, d.aux);
        if (k == K_DIR) push(new_lit("directory", 9));
        else if (k == K_LINK) push(new_lit("symlink", 7));
        else push(new_lit("regular", 7));
        ++n;
    }
    res_close(ref);
    nursery_reserve_n(n, ATTR_RESERVE, 4);
    set = attrs_alloc(n);
    for (i = 0; i < n; ++i) {
        windows_maybe();                        /* the set is young: no view lives past one */
        insert_entry_pos(set, n, &cnt, vstack[b + 2 * i], vstack[b + 2 * i + 1], 0);
    }
    vsp = b;
    push(set);
}

void prim_hashfile(void)                        /* hashFile type p */
{
    uint8_t algo, dir;
    if (!gsos_here()) prim_gsos(PRIM_HASHFILE, 2);
    force_slot(2);                              /* the type, first, and parsed */
    algo = hash_type(vstack[vsp - 2]);
    force_top();
    walk(path_arg(&dir), R_FULL);
    push_file(0);
    hash_top(algo);
    vstack[vsp - 2] = vstack[vsp - 1];          /* the digest where the type was */
    --vsp;
}

void prim_getenv(void)                          /* getEnv s: the ORCA shell's variable s */
{
    struct gs_readvar rv;
    obj_t *s;
    uint16_t n, len;
    if (!gsos_here()) prim_gsos(PRIM_GETENV, 1);
    force_top();
    s = deref(peek());
    need(s, T_STRING);
    if (CTX(s)) type_err();                     /* forceStringNoCtx */
    n = AUX16(s);
    if (n > G_MAX) nomem();
    *(uint16_t *)G_RES = n;                     /* the name, a GSString */
    memcpy(G_RES + 2, STRBYTES(s), n);
    *(uint16_t *)G_TMP = G_MAX;                 /* the value, a ResultBuf */
    *(uint16_t *)(G_TMP + 2) = 0;
    rv.pcount = 2;
    rv.name = G_RES;
    rv.value = G_TMP;
    len = gs_call(0x010B, &rv) ? 0 : *(uint16_t *)(G_TMP + 2);   /* no shell: "" */
    --vsp;
    nursery_reserve(round4(len) + 8);
    s = str_alloc(len);
    memcpy(STRBYTES(s), G_TMP + 4, len);
    push(s);
}

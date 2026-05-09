/* krep_index.c - LMDB-backed trigram index for krep
 *
 * Three named databases:
 *   "files"    : filepath -> mtime_rec_t  (file metadata)
 *   "trigrams" : 3-byte key -> filepath   (MDB_DUPSORT, inverted index)
 *   "ftrigs"   : filepath -> N*3 bytes    (reverse map: file -> its trigrams)
 *
 * The reverse map "ftrigs" makes removal O(k) per file (k = unique trigrams)
 * instead of scanning all trigram entries.
 * Write transactions are batched (FILES_PER_TXN) to amortize LMDB overhead.
 */

#define _POSIX_C_SOURCE 200809L
#include "krep_index.h"
#include "lmdb/lmdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <strings.h>   /* strcasecmp */

/* ------------------------------------------------------------------ */
#define INDEX_SUBDIR     ".krep-index"
#define DB_FILES         "files"
#define DB_TRIGRAMS      "trigrams"
#define DB_FTRIGS        "ftrigs"
#define INDEX_MAP_SIZE   (512UL * 1024 * 1024)
#define TRIGRAM_LEN      3
#define MAX_READ_SIZE    (4 * 1024 * 1024)    /* index up to 4 MB per file */
#define FILES_PER_TXN    64                   /* files per write transaction */

typedef struct { int64_t mtime_sec; int32_t mtime_nsec; } mtime_rec_t;

struct krep_index {
    MDB_env *env;
    MDB_dbi  dbi_files;
    MDB_dbi  dbi_trigrams;
    MDB_dbi  dbi_ftrigs;
    char     dir[PATH_MAX];
};

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

static void make_index_path(const char *dir, char *out, size_t len)
{
    /* Strip trailing slashes for consistent path construction */
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen-1] == '/') dlen--;
    snprintf(out, len, "%.*s/%s", (int)dlen, dir, INDEX_SUBDIR);
}

static void stat_mtime(const struct stat *st, int64_t *sec, int32_t *nsec)
{
    *sec  = (int64_t)st->st_mtime;
    *nsec = 0;
    (void)nsec;
}

static bool looks_binary(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0') return true;
    return false;
}

static bool skip_dir_name(const char *name)
{
    static const char *skip[] = {
        ".git",".svn",".hg",".krep-index","node_modules",
        ".cache","build","dist","__pycache__",".tox","target",NULL
    };
    for (int i = 0; skip[i]; i++)
        if (strcmp(name, skip[i]) == 0) return true;
    return false;
}

static bool skip_extension(const char *name)
{
    static const char *exts[] = {
        ".o",".a",".so",".dylib",".dll",".exe",".obj",
        ".pyc",".pyo",".class",
        ".zip",".gz",".bz2",".xz",".zst",".tar",
        ".jpg",".jpeg",".png",".gif",".bmp",".ico",".webp",
        ".pdf",".mp3",".mp4",".avi",".mkv",".mov",
        ".db",".sqlite",".mdb",".ldb",NULL
    };
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; exts[i]; i++)
        if (strcasecmp(dot, exts[i]) == 0) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Trigram extraction                                                  */
/* ------------------------------------------------------------------ */

#define ALPHA_N 95   /* printable ASCII 0x20..0x7E */
#define SEEN_BYTES ((ALPHA_N*ALPHA_N*ALPHA_N+7)/8)

typedef struct { uint8_t b[SEEN_BYTES]; } tgset_t;

static void tgset_init(tgset_t *s) { memset(s->b, 0, SEEN_BYTES); }

static bool tgset_mark(tgset_t *s, int idx) {
    if (idx < 0) return true;
    if (s->b[idx>>3] & (1u<<(idx&7))) return true;
    s->b[idx>>3] |= (1u<<(idx&7));
    return false;
}

static inline int trigram_idx(const unsigned char *t)
{
    int a = t[0]-0x20, b = t[1]-0x20, c = t[2]-0x20;
    if (a<0||a>=ALPHA_N||b<0||b>=ALPHA_N||c<0||c>=ALPHA_N) return -1;
    return a*ALPHA_N*ALPHA_N + b*ALPHA_N + c;
}

/*
 * Extract unique lowercase printable-ASCII trigrams from buf[0..len).
 * Returns heap blob of n*3 bytes. Sets *out_n. Caller must free().
 */
static char *extract_trigrams_blob(const char *buf, size_t len, size_t *out_n)
{
    *out_n = 0;
    if (len < (size_t)TRIGRAM_LEN) return NULL;

    tgset_t *seen = calloc(1, sizeof(tgset_t));
    if (!seen) return NULL;
    tgset_init(seen);

    size_t cap = 256;
    char *blob = malloc(cap * TRIGRAM_LEN);
    if (!blob) { free(seen); return NULL; }

    size_t n = 0;
    for (size_t i = 0; i+TRIGRAM_LEN <= len; i++) {
        char tg[3];
        tg[0] = (char)tolower((unsigned char)buf[i]);
        tg[1] = (char)tolower((unsigned char)buf[i+1]);
        tg[2] = (char)tolower((unsigned char)buf[i+2]);
        int idx = trigram_idx((const unsigned char *)tg);
        if (!tgset_mark(seen, idx)) {
            if (n >= cap) {
                size_t nc = cap * 2;
                if (nc > (size_t)(ALPHA_N*ALPHA_N*ALPHA_N))
                    nc = (size_t)(ALPHA_N*ALPHA_N*ALPHA_N);
                char *tmp = realloc(blob, nc * TRIGRAM_LEN);
                if (!tmp) break;
                blob = tmp; cap = nc;
            }
            memcpy(blob + n*TRIGRAM_LEN, tg, TRIGRAM_LEN);
            n++;
        }
    }
    free(seen);
    *out_n = n;
    if (n == 0) { free(blob); return NULL; }
    return blob;
}

/* ------------------------------------------------------------------ */
/* Open LMDB environment                                               */
/* ------------------------------------------------------------------ */

static int open_env(const char *path, unsigned flags,
                    MDB_env **eout, MDB_dbi *df, MDB_dbi *dt, MDB_dbi *dft)
{
    MDB_env *env; MDB_txn *txn; int rc;
    if ((rc=mdb_env_create(&env)) != 0) {
        fprintf(stderr, "krep-index: mdb_env_create: %s\n", mdb_strerror(rc));
        return KRIDX_ERR_ENV;
    }
    mdb_env_set_maxdbs(env, 3);
    mdb_env_set_mapsize(env, INDEX_MAP_SIZE);
    if ((rc=mdb_env_open(env, path, flags, 0644)) != 0) {
        fprintf(stderr, "krep-index: cannot open %s: %s\n", path, mdb_strerror(rc));
        mdb_env_close(env); return KRIDX_ERR_ENV;
    }
    unsigned int txn_flags = (flags & MDB_RDONLY) ? MDB_RDONLY : 0;
    if ((rc=mdb_txn_begin(env, NULL, txn_flags, &txn)) != 0) {
        mdb_env_close(env); return KRIDX_ERR_ENV;
    }
    unsigned int db_open_flags = (flags & MDB_RDONLY) ? 0 : MDB_CREATE;
    if (mdb_dbi_open(txn, DB_FILES,    db_open_flags,             df)  != 0 ||
        mdb_dbi_open(txn, DB_TRIGRAMS, db_open_flags|MDB_DUPSORT, dt)  != 0 ||
        mdb_dbi_open(txn, DB_FTRIGS,   db_open_flags,             dft) != 0) {
        mdb_txn_abort(txn); mdb_env_close(env); return KRIDX_ERR_ENV;
    }
    if ((rc=mdb_txn_commit(txn)) != 0) {
        mdb_env_close(env); return KRIDX_ERR_ENV;
    }
    *eout = env;
    return KRIDX_OK;
}

/* ------------------------------------------------------------------ */
/* Remove one file using its stored reverse-map entry: O(k) not O(N)  */
/* ------------------------------------------------------------------ */

static void remove_file_from_index(MDB_txn *txn, MDB_dbi dbi_trigrams,
                                   MDB_dbi dbi_ftrigs, const char *filepath)
{
    size_t fplen = strlen(filepath)+1;
    MDB_val kf = { fplen, (void*)filepath };
    MDB_val vf;

    if (mdb_get(txn, dbi_ftrigs, &kf, &vf) != 0) return; /* not in index */

    size_t ntg = vf.mv_size / TRIGRAM_LEN;
    const char *tgblob = (const char *)vf.mv_data;

    MDB_cursor *cur = NULL;
    if (mdb_cursor_open(txn, dbi_trigrams, &cur) != 0) return;

    for (size_t i = 0; i < ntg; i++) {
        MDB_val k = { TRIGRAM_LEN, (void*)(tgblob + i*TRIGRAM_LEN) };
        MDB_val v = { fplen,       (void*)filepath };
        if (mdb_cursor_get(cur, &k, &v, MDB_GET_BOTH) == 0)
            mdb_cursor_del(cur, 0);
    }
    mdb_cursor_close(cur);
    mdb_del(txn, dbi_ftrigs, &kf, NULL);
}

/* ------------------------------------------------------------------ */
/* Batch write context                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    MDB_env *env;
    MDB_dbi  dfi, dft, dfr;
    MDB_txn *txn;
    int      count;
    size_t   indexed;
    size_t   skipped;
} batch_t;

static int batch_begin(batch_t *b)
{
    int rc = mdb_txn_begin(b->env, NULL, 0, &b->txn);
    if (rc != 0) { b->txn = NULL; }
    return rc;
}

static int batch_commit(batch_t *b)
{
    if (!b->txn) return 0;
    int rc = mdb_txn_commit(b->txn);
    b->txn = NULL; b->count = 0;
    return rc;
}

/* Index one file inside the current write transaction */
static void index_one_in_txn(batch_t *b, const char *filepath, const struct stat *st)
{
    int64_t ms; int32_t mn;
    stat_mtime(st, &ms, &mn);
    size_t fplen = strlen(filepath)+1;

    MDB_val kf = { fplen, (void*)filepath }, vf;

    /* Check if already up-to-date */
    if (mdb_get(b->txn, b->dfi, &kf, &vf) == 0 &&
        vf.mv_size >= sizeof(mtime_rec_t)) {
        const mtime_rec_t *r = (const mtime_rec_t*)vf.mv_data;
        if (r->mtime_sec == ms) { b->skipped++; return; } /* no change */
        remove_file_from_index(b->txn, b->dft, b->dfr, filepath);
    }

    /* Read file content */
    FILE *f = fopen(filepath, "rb");
    if (!f) { b->skipped++; return; }
    char *buf = malloc(MAX_READ_SIZE);
    if (!buf) { fclose(f); b->skipped++; return; }
    size_t blen = fread(buf, 1, MAX_READ_SIZE, f);
    fclose(f);

    /* Extract trigrams */
    size_t ntg = 0;
    char *tgblob = extract_trigrams_blob(buf, blen, &ntg);
    free(buf);

    /* Update files DB */
    mtime_rec_t rec = { ms, mn };
    vf.mv_size = sizeof(rec); vf.mv_data = &rec;
    mdb_put(b->txn, b->dfi, &kf, &vf, 0);

    if (tgblob && ntg > 0) {
        /* Store reverse map: file -> trigrams blob */
        MDB_val vblob = { ntg * TRIGRAM_LEN, tgblob };
        mdb_put(b->txn, b->dfr, &kf, &vblob, 0);

        /* Inverted index: trigram -> filepath */
        for (size_t i = 0; i < ntg; i++) {
            MDB_val k = { TRIGRAM_LEN, tgblob + i*TRIGRAM_LEN };
            MDB_val v = { fplen,       (void*)filepath };
            mdb_put(b->txn, b->dft, &k, &v, MDB_NODUPDATA);
        }
        free(tgblob);
    }

    b->indexed++;
    b->count++;

    if (b->count >= FILES_PER_TXN) {
        batch_commit(b);
        batch_begin(b);
    }
}

/* ------------------------------------------------------------------ */
/* Prune deleted files                                                 */
/* ------------------------------------------------------------------ */

static void prune_deleted(MDB_env *env, MDB_dbi dfi, MDB_dbi dft, MDB_dbi dfr)
{
    MDB_txn *rtxn = NULL;
    if (mdb_txn_begin(env, NULL, MDB_RDONLY, &rtxn) != 0) return;
    MDB_cursor *cur = NULL;
    if (mdb_cursor_open(rtxn, dfi, &cur) != 0) { mdb_txn_abort(rtxn); return; }

    char **del = NULL; size_t dcap=0, dlen=0;
    MDB_val k, v;
    while (mdb_cursor_get(cur, &k, &v, MDB_NEXT) == 0) {
        struct stat st;
        if (lstat((const char*)k.mv_data, &st) != 0) {
            char *cp = strndup((const char*)k.mv_data, k.mv_size);
            if (!cp) continue;
            if (dlen >= dcap) {
                dcap = dcap ? dcap*2 : 64;
                char **tmp = realloc(del, dcap * sizeof(char*));
                if (!tmp) { free(cp); continue; }
                del = tmp;
            }
            del[dlen++] = cp;
        }
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(rtxn);

    for (size_t i = 0; i < dlen; i++) {
        MDB_txn *w = NULL;
        if (mdb_txn_begin(env, NULL, 0, &w) == 0) {
            remove_file_from_index(w, dft, dfr, del[i]);
            MDB_val kd = { strlen(del[i])+1, del[i] };
            mdb_del(w, dfi, &kd, NULL);
            mdb_txn_commit(w);
        }
        free(del[i]);
    }
    free(del);
}

/* ------------------------------------------------------------------ */
/* Recursive walker                                                    */
/* ------------------------------------------------------------------ */

static void walk(batch_t *b, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[PATH_MAX];

    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name,".")==0 || strcmp(e->d_name,"..")==0) continue;

        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;

        struct stat st;
        if (lstat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!skip_dir_name(e->d_name)) walk(b, path);
        } else if (S_ISREG(st.st_mode)) {
            if (skip_extension(e->d_name) || st.st_size == 0) {
                b->skipped++; continue;
            }
            if (st.st_size < 512*1024 && looks_binary(path)) {
                b->skipped++; continue;
            }
            if (!b->txn && batch_begin(b) != 0) {
                b->skipped++; continue;
            }
            index_one_in_txn(b, path, &st);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* Public: build                                                       */
/* ------------------------------------------------------------------ */

int krep_index_build(const char *dir)
{
    /* Normalize: strip trailing slashes */
    char norm_dir[PATH_MAX];
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen-1] == '/') dlen--;
    snprintf(norm_dir, sizeof(norm_dir), "%.*s", (int)dlen, dir);
    dir = norm_dir;

    char index_path[PATH_MAX];
    make_index_path(dir, index_path, sizeof(index_path));

    if (mkdir(index_path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "krep-index: mkdir %s: %s\n", index_path, strerror(errno));
        return KRIDX_ERR_IO;
    }

    MDB_env *env; MDB_dbi dfi, dft, dfr;
    if (open_env(index_path, 0, &env, &dfi, &dft, &dfr) != KRIDX_OK)
        return KRIDX_ERR_ENV;

    fprintf(stderr, "krep-index: building index for '%s' ...\n", dir);
    prune_deleted(env, dfi, dft, dfr);

    batch_t b = { env, dfi, dft, dfr, NULL, 0, 0, 0 };
    walk(&b, dir);
    if (b.txn) batch_commit(&b);

    fprintf(stderr, "krep-index: done. indexed=%zu skipped=%zu  (%s)\n",
            b.indexed, b.skipped, index_path);
    mdb_env_close(env);
    return KRIDX_OK;
}

/* ------------------------------------------------------------------ */
/* Public: open / close                                                */
/* ------------------------------------------------------------------ */

krep_index_t *krep_index_open(const char *dir)
{
    /* Normalize: strip trailing slashes */
    char norm_dir[PATH_MAX];
    size_t dlen = strlen(dir);
    while (dlen > 1 && dir[dlen-1] == '/') dlen--;
    snprintf(norm_dir, sizeof(norm_dir), "%.*s", (int)dlen, dir);
    dir = norm_dir;

    char index_path[PATH_MAX];
    make_index_path(dir, index_path, sizeof(index_path));

    struct stat st;
    if (stat(index_path, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;

    MDB_env *env; MDB_dbi dfi, dft, dfr;
    if (open_env(index_path, MDB_RDONLY, &env, &dfi, &dft, &dfr) != KRIDX_OK)
        return NULL;

    krep_index_t *idx = malloc(sizeof(krep_index_t));
    if (!idx) { mdb_env_close(env); return NULL; }
    idx->env = env; idx->dbi_files = dfi;
    idx->dbi_trigrams = dft; idx->dbi_ftrigs = dfr;
    strncpy(idx->dir, dir, sizeof(idx->dir)-1);
    idx->dir[sizeof(idx->dir)-1] = '\0';
    return idx;
}

void krep_index_close(krep_index_t *idx)
{
    if (!idx) return;
    mdb_env_close(idx->env);
    free(idx);
}

/* ------------------------------------------------------------------ */
/* Candidate hash set                                                  */
/* ------------------------------------------------------------------ */

#define CSET_SZ 8192
typedef struct cse { char *path; struct cse *next; } cse_t;
typedef struct { cse_t *b[CSET_SZ]; size_t n; } cset_t;

static void cs_init(cset_t *s) { memset(s->b, 0, sizeof(s->b)); s->n = 0; }

static uint32_t cs_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static bool cs_has(const cset_t *s, const char *p) {
    uint32_t h = cs_hash(p) & (CSET_SZ-1);
    for (cse_t *e = s->b[h]; e; e = e->next)
        if (strcmp(e->path, p) == 0) return true;
    return false;
}

static bool cs_add(cset_t *s, char *owned) {
    uint32_t h = cs_hash(owned) & (CSET_SZ-1);
    for (cse_t *e = s->b[h]; e; e = e->next)
        if (strcmp(e->path, owned) == 0) return true;
    cse_t *e = malloc(sizeof(cse_t));
    if (!e) return false;
    e->path = owned; e->next = s->b[h]; s->b[h] = e; s->n++;
    return true;
}

static void cs_free(cset_t *s) {
    for (int i = 0; i < CSET_SZ; i++) {
        cse_t *e = s->b[i];
        while (e) { cse_t *nx = e->next; free(e->path); free(e); e = nx; }
        s->b[i] = NULL;
    }
    s->n = 0;
}

static void cs_fill_trigram(krep_index_t *idx, const char *tg, cset_t *out)
{
    MDB_txn *txn = NULL;
    if (mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &txn) != 0) return;
    MDB_cursor *c = NULL;
    if (mdb_cursor_open(txn, idx->dbi_trigrams, &c) != 0) {
        mdb_txn_abort(txn); return;
    }
    MDB_val k = { (size_t)TRIGRAM_LEN, (void*)tg }, v;
    int rc = mdb_cursor_get(c, &k, &v, MDB_SET_KEY);
    while (rc == 0) {
        char *cp = strndup((const char*)v.mv_data, v.mv_size);
        if (cp) cs_add(out, cp);
        rc = mdb_cursor_get(c, &k, &v, MDB_NEXT_DUP);
    }
    mdb_cursor_close(c);
    mdb_txn_abort(txn);
}

static void cs_intersect_trigram(krep_index_t *idx, const char *tg, cset_t *cands)
{
    if (cands->n == 0) return;
    cset_t next; cs_init(&next);
    MDB_txn *txn = NULL;
    if (mdb_txn_begin(idx->env, NULL, MDB_RDONLY, &txn) != 0) return;
    MDB_cursor *c = NULL;
    if (mdb_cursor_open(txn, idx->dbi_trigrams, &c) != 0) {
        mdb_txn_abort(txn); return;
    }
    MDB_val k = { (size_t)TRIGRAM_LEN, (void*)tg }, v;
    int rc = mdb_cursor_get(c, &k, &v, MDB_SET_KEY);
    while (rc == 0) {
        const char *p = (const char*)v.mv_data;
        if (cs_has(cands, p)) {
            char *cp = strndup(p, v.mv_size);
            if (cp) cs_add(&next, cp);
        }
        rc = mdb_cursor_get(c, &k, &v, MDB_NEXT_DUP);
    }
    mdb_cursor_close(c);
    mdb_txn_abort(txn);
    cs_free(cands);
    *cands = next;
}

/* ------------------------------------------------------------------ */
/* Public: query                                                       */
/* ------------------------------------------------------------------ */

int krep_index_query(krep_index_t *idx,
                     const char *pattern, size_t pattern_len,
                     bool case_sensitive,
                     char **out_paths, int max_results,
                     bool *unindexed_out)
{
    (void)case_sensitive;
    if (unindexed_out) *unindexed_out = false;

    if (pattern_len < (size_t)TRIGRAM_LEN) {
        if (unindexed_out) *unindexed_out = true;
        return 0;
    }

    char *lp = malloc(pattern_len + 1);
    if (!lp) return -1;
    for (size_t i = 0; i < pattern_len; i++)
        lp[i] = (char)tolower((unsigned char)pattern[i]);
    lp[pattern_len] = '\0';

#define MAX_PAT_TG 1024
    char tgs[MAX_PAT_TG][4];
    int  ntg = 0;
    tgset_t *seen = calloc(1, sizeof(tgset_t));
    if (!seen) { free(lp); return -1; }
    tgset_init(seen);

    for (size_t i = 0; i+TRIGRAM_LEN <= pattern_len && ntg < MAX_PAT_TG; i++) {
        char tg[3] = { lp[i], lp[i+1], lp[i+2] };
        int ii = trigram_idx((const unsigned char*)tg);
        if (!tgset_mark(seen, ii)) {
            memcpy(tgs[ntg], tg, 3); tgs[ntg][3] = '\0'; ntg++;
        }
    }
    free(seen); free(lp);

    if (ntg == 0) { if (unindexed_out) *unindexed_out = true; return 0; }

    cset_t cands; cs_init(&cands);
    cs_fill_trigram(idx, tgs[0], &cands);
    for (int t = 1; t < ntg && cands.n > 0; t++)
        cs_intersect_trigram(idx, tgs[t], &cands);

    int nr = 0;
    for (int bi = 0; bi < CSET_SZ && nr < max_results; bi++) {
        for (cse_t *e = cands.b[bi]; e && nr < max_results; e = e->next) {
            struct stat st;
            if (lstat(e->path, &st) != 0) continue;
            out_paths[nr] = strdup(e->path);
            if (!out_paths[nr]) break;
            nr++;
        }
    }
    cs_free(&cands);
    return nr;
}

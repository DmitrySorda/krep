/* krep_index.h - LMDB-backed trigram index for krep
 *
 * Provides build/update/query interface for an on-disk trigram index that
 * accelerates repeated recursive searches by pre-filtering candidate files.
 *
 * Index layout (stored in <dir>/.krep-index/):
 *   DB "files"   : key = filepath (NUL-terminated), value = {int64 mtime_sec, int32 mtime_nsec}
 *   DB "trigrams": key = 3-byte trigram, value = filepath (MDB_DUPSORT, each dup = one file)
 *
 * Validation: on each query, mtime of each candidate is verified against stored value.
 * Invalidation: --build-index rescans changed/new files and removes stale trigram entries.
 */

#ifndef KREP_INDEX_H
#define KREP_INDEX_H

#include <stdbool.h>
#include <stddef.h>

/* Return codes */
#define KRIDX_OK          0
#define KRIDX_ERR_ENV     1  /* LMDB environment error */
#define KRIDX_ERR_IO      2  /* filesystem error */
#define KRIDX_ERR_MEM     3  /* memory allocation failure */
#define KRIDX_ERR_NOINDEX 4  /* index does not exist */

/* Maximum number of candidate files returned by a query */
#define KRIDX_MAX_CANDIDATES 65536

/* Opaque index handle */
typedef struct krep_index krep_index_t;

/*
 * krep_index_build - build or update the trigram index for the given directory.
 *
 * Creates <dir>/.krep-index/ if it does not exist.  Scans all text files
 * recursively, updating entries whose mtime has changed and removing entries
 * for deleted files.
 *
 * Returns KRIDX_OK on success, one of the KRIDX_ERR_* codes otherwise.
 * Prints progress/error messages to stderr.
 */
int krep_index_build(const char *dir);

/*
 * krep_index_open - open an existing index for reading (used during search).
 *
 * Returns a heap-allocated handle on success, NULL if the index does not
 * exist or cannot be opened.  Call krep_index_close() when done.
 */
krep_index_t *krep_index_open(const char *dir);

/*
 * krep_index_close - close and free an index handle.
 */
void krep_index_close(krep_index_t *idx);

/*
 * krep_index_query - find candidate files that may contain all trigrams of pattern.
 *
 * Writes at most max_results file paths into out_paths[].  Each entry is a
 * heap-allocated string that the caller must free().
 *
 * unindexed_out: if non-NULL, set to true when the pattern is too short for
 *               trigram filtering (len < 3) — caller should fall back to full scan.
 *
 * Returns the number of candidates written, or -1 on error.
 */
int krep_index_query(krep_index_t *idx,
                     const char   *pattern,
                     size_t        pattern_len,
                     bool          case_sensitive,
                     char        **out_paths,
                     int           max_results,
                     bool         *unindexed_out);

#endif /* KREP_INDEX_H */

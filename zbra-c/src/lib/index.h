/*
 * index.h -- the unified package index.
 *
 * zbra supports five payload formats, but the resolver and database want one
 * shape of record. This module is the translation layer: it reads each
 * ecosystem's index format and produces identical zbra_package records, so
 * that everything above it is format-agnostic.
 *
 * The index formats actually read:
 *
 *   deb     Debian "Packages" stanzas. Sources: apt repositories, and local
 *           directories produced by scripts/generate-repo.sh.
 *
 *   rpm     RPM primary.xml / repomd.xml, reduced to the fields zbra needs.
 *           Implementing full XML repodata parsing is out of scope; zbra
 *           prefers a repomd.xml.summary-shaped cache when available and
 *           otherwise hands the repository to dnf, which already solves
 *           primary.xml correctly.
 *
 *   tar.xz  zbra's own index.json, a flat array of records. This is what
 *           "zbra-native" repositories publish, and the format used by the
 *           TenebraOS packages the ISO ships.
 *
 *   snap    Delegated to snapd.
 *
 *   aur     Delegated to makepkg; the AUR has no machine-readable index that
 *           can be fetched without an RPC round trip per package, so zbra
 *           resolves those names at install time instead.
 *
 * Index files are attacker-controlled input: they come from remote servers
 * and name packages and files that zbra will act on. Every name and path is
 * run through zbra_name_is_safe / zbra_path_is_safe before being stored,
 * and a record failing validation is dropped with a warning rather than
 * trusted, because one hostile index entry must not be able to make zbra
 * write outside the database or the install root.
 */

#ifndef ZBRA_INDEX_H
#define ZBRA_INDEX_H

#include <stddef.h>

#include "vercmp.h"
#include "deps.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One package as the rest of zbra sees it. */
typedef struct {
    char            *name;
    char            *version;
    char            *arch;            /* "amd64", "all", or NULL */
    char            *format;          /* "deb", "rpm", "tar.xz", "snap", "aur" */
    char            *source_id;       /* which source offered it */
    char            *source_url;
    zbra_ver_style   style;           /* version ordering for `format` */

    char            *filename;        /* relative or absolute payload location */
    char            *checksum;        /* "sha256:<hex>", or NULL */
    long long        size;            /* bytes, 0 when unknown */

    char            *description;
    char            *homepage;
    char            *license;

    char           **depends;         /* raw requirement strings */
    size_t           n_depends;

    /* Payload contents, when the index knew them without unpacking. */
    char           **conflicts;
    size_t           n_conflicts;
    char           **provides;
    size_t           n_provides;
    char           **replaces;
    size_t           n_replaces;
} zbra_package;

void zbra_package_free(zbra_package *p);
void zbra_package_list_free(zbra_package *list, size_t n);

/* --------------------------------------------------------------- the index */

typedef struct {
    zbra_package *items;
    size_t        n;
    size_t        cap;
    char         **warnings;          /* dropped records, reported by the CLI */
    size_t        n_warnings;
} zbra_index;

void zbra_index_init(zbra_index *idx);
void zbra_index_free(zbra_index *idx);

/* Record a dropped record so `zbra update` can report it. */
void zbra_index_warn(zbra_index *idx, const char *fmt, ...);

/*
 * Read a Debian "Packages" file. `path` may be "-" for stdin.
 * Returns 0 on success, -1 on failure with *err.
 */
int zbra_index_read_deb_packages(zbra_index *idx, const char *path,
                                 const char *source_id, const char *source_url,
                                 char **err);

/*
 * Read zbra's own index.json for a tar.xz source.
 */
int zbra_index_read_json(zbra_index *idx, const char *path,
                         const char *source_id, const char *source_url,
                         char **err);

/*
 * Read an index from a downloaded file, choosing the parser by extension.
 * Unknown extensions are an error rather than a guess.
 */
int zbra_index_read_auto(zbra_index *idx, const char *path,
                         const char *source_id, const char *source_url,
                         char **err);

/* --------------------------------------------------------------- queries */

/*
 * Best available version of `name`, honouring each record's own version
 * style. Returns 1 and fills *out, or 0 when absent, -1 on error.
 */
int zbra_index_find(const zbra_index *idx, const char *name,
                    zbra_package *out);

/*
 * Candidate lookup for deps.c.
 *
 * Wraps zbra_index_find and fills a zbra_candidate, so the resolver stays
 * independent of this module. `ud` is the zbra_index. The signature matches
 * zbra_resolver.find_candidate exactly, so it can be assigned straight into
 * the resolver struct rather than wrapped in a shim.
 *
 * Returns 1 and fills *out (caller frees with zbra_candidate_free), 0 when
 * no source offers the package, or -1 on error.
 */
int zbra_index_find_candidate(void *ud, const char *name,
                              zbra_candidate *out);

/*
 * Substring search over name, description and source.
 *
 * `max` is the capacity of `out`, and is always honoured: a caller with a
 * fixed-size array must never be overrun, so `max` is never treated as
 * "unlimited". Pass NULL for `out` to count matches without collecting them,
 * which lets a caller size a buffer before filling it.
 *
 * Returns the number of matches when `out` is NULL, otherwise the number
 * stored (at most `max`), sorted so the closest match to `needle` comes
 * first.
 */
size_t zbra_index_search(const zbra_index *idx, const char *needle,
                         zbra_package **out, size_t max);

/* --------------------------------------------------------------- building */

/*
 * Sort an index so the same input always yields the same listing, which
 * keeps `zbra search` output diffable and reproducible.
 */
void zbra_index_sort(zbra_index *idx);

/* Merge `src` into `dst`, keeping the higher version on conflict. */
int zbra_index_merge(zbra_index *dst, const zbra_index *src);

/*
 * Write an index as index.json. Used to publish tar.xz sources and by
 * `zbra build` to produce a repository from a directory of packages.
 */
int zbra_index_write_json(const zbra_index *idx, const char *path, char **err);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_INDEX_H */
/*
 * db.h -- the installed-package database.
 *
 * zbra keeps its own database rather than deferring entirely to dpkg,
 * because a multi-format manager needs to answer questions dpkg cannot:
 *
 *   - which files does THIS package own, so removal touches nothing else?
 *   - which installed packages still need this one as a dependency, so it
 *     is safe to remove?
 *   - which distro did this package come from, so two distros' copies of the
 *     same library can coexist (the Bedrock-style isolation feature)?
 *
 * The layout mirrors ZETA's, which is a proven design:
 *
 *   <root>/packages/<name>/      explicitly requested by the user
 *   <root>/dependencies/<name>/   installed only to satisfy something else
 *   <root>/isolated/<name>/       installed into its own prefix so that it
 *                                 cannot collide with another distro's files
 *
 * Keeping the two registries separate is what makes "was this asked for, or
 * did something else drag it in?" answerable, which in turn decides whether
 * `zbra remove` should remove it or merely suggest it.
 *
 * Each entry directory contains:
 *
 *   meta   one "field: value" line per attribute, human-readable on purpose
 *          so a corrupted database can be repaired by hand
 *   files  one owned path per line, relative to the install root. This list
 *          is the authoritative record of what removal may unlink.
 *
 * A dependency's meta additionally carries a "dependents:" list naming the
 * installed entries that require it. That back-reference is what allows
 * --with-deps to cascade safely: removing a package can consult its
 * dependents to discover what would break, rather than deleting blindly.
 */

#ifndef ZBRA_DB_H
#define ZBRA_DB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which registry an entry belongs to. */
typedef enum {
    ZBRA_KIND_PACKAGE = 0,     /* explicitly installed by the user */
    ZBRA_KIND_DEPENDENCY = 1,  /* installed automatically */
    ZBRA_KIND_ISOLATED = 2     /* installed into a private prefix */
} zbra_kind;

/*
 * Passed to zbra_db_list to mean "every registry". It is not a real kind
 * and is never written to disk; naming it explicitly beats the alternative
 * of casting -1 at the parameter and having the reader wonder.
 */
#define ZBRA_KIND_ANY ((zbra_kind)-1)

/* One installed package. */
typedef struct {
    char      *name;
    char      *version;
    char      *source;         /* id of the source it came from */
    char      *format;         /* "deb", "rpm", "tar.xz", "snap", "aur" */
    char      *arch;           /* "amd64", "all", or NULL */
    char      *install_root;   /* non-NULL only for isolated entries */

    char     **depends;        /* requirement strings, as written by the index */
    size_t     n_depends;

    char     **dependents;     /* installed entries that need this package */
    size_t     n_dependents;

    char     **files;          /* owned paths, relative to install_root */
    size_t     n_files;

    zbra_kind  kind;
} zbra_entry;

/* An open database rooted at a directory (normally /var/lib/zbra). */
typedef struct {
    char *root;
} zbra_db;

/*
 * Open (and create if absent) a database at `root`. Returns 0 on success,
 * -1 on failure with errno set.
 */
int zbra_db_open(zbra_db *db, const char *root);
void zbra_db_close(zbra_db *db);

/* The conventional on-disk location, honouring $ZBRA_ROOT for testing. */
const char *zbra_db_default_root(void);

/* --------------------------------------------------------------- */
/* Entry lifecycle                                                 */
/* --------------------------------------------------------------- */

/*
 * Write an entry, creating its directory and replacing any existing record.
 * The caller keeps ownership of the entry and its strings. Returns 0, or -1
 * with errno set.
 */
int zbra_db_put(zbra_db *db, const zbra_entry *e);

/*
 * Load one entry. Looks in all three registries, preferring an explicit
 * package over a dependency. On success fills *out with a heap entry that
 * the caller releases via zbra_entry_free.
 *
 * Returns 1 when found, 0 when absent, -1 on error.
 */
int zbra_db_get(zbra_db *db, const char *name, zbra_entry *out);

/*
 * Remove an entry's record (not its files -- see commit.c, which owns the
 * unlink step so that it can be journaled and rolled back).
 *
 * Returns 1 when something was removed, 0 when the name was unknown,
 * -1 on error.
 */
int zbra_db_forget(zbra_db *db, const char *name, zbra_kind kind);

/* Find which registry a name lives in; ZBRA_KIND_PACKAGE if not present. */
zbra_kind zbra_db_kind_of(zbra_db *db, const char *name);

/*
 * Record that `dependent` requires `name`, adding to the dependency's
 * dependents list idempotently. Returns 0 on success, -1 on error, and 1 if
 * `name` is not installed (nothing to record against).
 */
int zbra_db_add_dependent(zbra_db *db, const char *name, const char *dependent);

/*
 * Drop `dependent` from `name`'s dependents list, and remove the entry
 * entirely once nothing depends on it any more and it is not explicit.
 *
 * This is the single place that decides a dependency has become garbage.
 * Returns 1 if the entry was removed, 0 if it was kept, -1 on error.
 */
int zbra_db_release_dependent(zbra_db *db, const char *name,
                              const char *dependent);

/* --------------------------------------------------------------- */
/* Queries                                                         */
/* --------------------------------------------------------------- */

/*
 * Collect every installed name into a NULL-terminated array, optionally
 * restricted to one registry (pass all three to search them). Release with
 * zbra_strlist_free.
 */
char **zbra_db_list(zbra_db *db, zbra_kind kind, size_t *n);

/*
 * Find the first installed package that owns `path`. Returns the owning
 * name (heap) or NULL. This is how `zbra verify` answers "who put this file
 * here?", and how the resolver detects a file collision between distros.
 */
char *zbra_db_owner_of(zbra_db *db, const char *path);

void zbra_entry_free(zbra_entry *e);

/*
 * Free only the array itself, leaving the elements alone. Use this when the
 * array holds pointers the caller already owns (a static table, or a struct
 * field), NOT for a list built by zbra_db_list.
 */
void zbra_strlist_free(char **list);

/*
 * Free a NULL-terminated array AND every string in it. Use this for anything
 * returned by zbra_db_list, whose elements zbra allocated with strdup.
 */
void zbra_strlist_free_owned(char **list);

/*
 * True if `name` is a syntactically acceptable package name.
 *
 * This is a security boundary, not a cosmetic check: names are used to build
 * filesystem paths, so a name containing "../" or a leading slash could make
 * zbra read or write outside the database. The index reader runs every name
 * from a remote repository through this before it is stored.
 */
int zbra_name_is_safe(const char *name);

/* Same check for a file path that a package claims to own. */
int zbra_path_is_safe(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_DB_H */
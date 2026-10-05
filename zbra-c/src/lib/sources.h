/*
 * sources.h -- repository source configuration.
 *
 * zbra reads its list of sources from /etc/zbra/sources.list and any
 * /etc/zbra/sources.list.d/"*.list" files. The format is deliberately
 * a superset of apt's one-line form, because a multi-format manager has
 * to express things apt has no syntax for:
 *
 *     tenebra   deb     https://itzr1g.github.io/TenebraOS-packages/  ./
 *     fedora    rpm     https://example.org/fedora/39/x86_64/
 *     arch      aur
 *     core      snap    https://snapcraft.io
 *     local     tar.xz  file:///srv/mirror/pkgs
 *
 * Fields: <id> <type> <url> [suite] [components...]
 *
 * The type is what selects the backend: deb, rpm, tar.xz, snap, or aur. A
 * line whose type zbra does not recognise is reported rather than silently
 * ignored, because a source that quietly does nothing is how a user ends up
 * wondering why a package "doesn't exist".
 *
 * `zbra --edit-sources` opens $EDITOR on this file and re-reads it, so the
 * file must stay human-editable: comments with '#', blank lines ignored,
 * stable ordering.
 */

#ifndef ZBRA_SOURCES_H
#define ZBRA_SOURCES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Payload/backend families a source can serve. */
typedef enum {
    ZBRA_SRC_DEB = 0,
    ZBRA_SRC_RPM,
    ZBRA_SRC_TARXZ,
    ZBRA_SRC_SNAP,
    ZBRA_SRC_AUR
} zbra_source_type;

/* One configured source. */
typedef struct {
    char             *id;
    zbra_source_type  type;
    char             *url;
    char            **components;   /* apt suite/distribution, if any */
    size_t            n_components;
    int               enabled;
    char             *file;         /* file it came from, for diagnostics */
    int               line;         /* line number, for diagnostics */
} zbra_source;

/* Parse a single line. Returns 0 on success, -1 if malformed (with *err). */
int zbra_source_parse_line(const char *line, zbra_source *out, char **err);

/* Release one source's heap members. */
void zbra_source_free(zbra_source *s);

/* The loaded configuration. */
typedef struct {
    zbra_source *items;
    size_t       n;
    size_t       cap;
} zbra_sources;

void zbra_sources_init(zbra_sources *s);
void zbra_sources_free(zbra_sources *s);

/*
 * Read sources.list and sources.list.d/"*.list" under `dir` (normally
 * /etc/zbra). Returns 0 on success, -1 on failure with *err.
 *
 * Unknown types are skipped with a note appended to *warnings rather than
 * failing the whole load, so one stale line cannot make zbra unusable.
 */
int zbra_sources_load(zbra_sources *s, const char *dir, char **err,
                      char **warnings);

/* Serialise back to sources.list in stable order. */
int zbra_sources_save(const zbra_sources *s, const char *dir, char **err);

/* Add or replace by id. Returns 0, or -1 if the definition is malformed. */
int zbra_sources_set(zbra_sources *s, const char *id, const char *type,
                     const char *url, const char *const *components,
                     size_t n_components, char **err);

/* Find by id, or NULL. */
zbra_source *zbra_sources_find(zbra_sources *s, const char *id);

/*
 * Remove by id. Returns 1 when removed, 0 when the id was unknown. Removing
 * a source does not remove already-installed packages, since those are
 * tracked in the database and must remain removable.
 */
int zbra_sources_remove(zbra_sources *s, const char *id);

/*
 * Open $EDITOR (or $VISUAL, falling back to a default) on the sources file
 * and re-read it afterwards. `dir` is the configuration directory.
 * Returns 0 if the file parses after editing, -1 otherwise.
 */
int zbra_sources_edit(zbra_sources *s, const char *dir, char **err);

/* Human-readable type name, for display and for round-tripping. */
const char *zbra_source_type_name(zbra_source_type t);

/* Inverse of zbra_source_type_name. Returns -1 when unrecognised. */
int zbra_source_type_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_SOURCES_H */
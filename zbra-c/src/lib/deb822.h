/*
 * deb822.h -- parser for RFC822-style control stanzas.
 *
 * Every package index format zbra reads is this shape:
 *
 *   - Debian "Packages" and "Sources" files (also .changes, Release)
 *   - the control files inside a .deb (DEBIAN/control)
 *   - RPM .spec files (Name/Version/Release/Summary)
 *   - RPM primary.xml, written as XML but carrying the same field/value
 *     structure that this parser normalises upstream of the index layer
 *
 * A stanza is a sequence of "Field: value" lines. A line beginning with a
 * space or tab continues the previous field, which is how a long Depends
 * list is folded across lines:
 *
 *   Package: vim
 *   Version: 9.1-2
 *   Depends: libc6 (>= 2.38),
 *            libacl1 (>= 2.3),
 *            added-pkg
 *
 * The parser is streaming rather than "read the whole file into a tree"
 * because a real Packages index is tens of megabytes and hundreds of
 * thousands of stanzas; holding all of that at once is wasteful when the
 * caller only ever needs one stanza at a time.
 */

#ifndef ZBRA_DEB822_H
#define ZBRA_DEB822_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One "Field: value" pair. */
typedef struct zbra_d822_field {
    char *key;                      /* as written in the file */
    char *val;                      /* continuation lines already folded in */
    struct zbra_d822_field *next;
} zbra_d822_field;

/*
 * A reader over a deb822 stream. Initialise with zbra_d822_open, advance
 * with zbra_d822_next, and release with zbra_d822_close.
 */
typedef struct {
    FILE          *fp;
    char          *line;            /* getline() scratch buffer */
    size_t         cap;
    zbra_d822_field *head;          /* fields of the current stanza */
    zbra_d822_field *tail;
    long           lineno;
    int            eof;
} zbra_d822;

/*
 * Attach a reader to an open stream. Returns 0 on success, -1 on failure
 * (errno set). Ownership of `fp` stays with the caller.
 */
int zbra_d822_open(zbra_d822 *r, FILE *fp);

/*
 * Read the next stanza into the reader, discarding any previous one.
 *
 * Returns  1 when a stanza was read (possibly with zero fields, if the
 *          file contains blank-line-separated empty stanzas),
 *          0 at end of input,
 *         -1 on a read error or a line that is neither a field nor a
 *          continuation (set r->lineno to the offending line).
 */
int zbra_d822_next(zbra_d822 *r);

/* Drop the current stanza's fields but keep the stream position. */
void zbra_d822_reset(zbra_d822 *r);

/* Free the reader's buffers. Does NOT close `fp`. */
void zbra_d822_close(zbra_d822 *r);

/*
 * Case-insensitive lookup, because the formats disagree on capitalisation
 * ("Package" in Debian, "Name" in RPM). Returns NULL when absent.
 */
const char *zbra_d822_get(const zbra_d822 *r, const char *key);
int zbra_d822_has(const zbra_d822 *r, const char *key);

/*
 * Field names can repeat in some formats (a .spec may list several
 * Source lines, and control files may repeat fields for multi-binary
 * packages). Return the value at `idx` (0-based) or NULL.
 */
const char *zbra_d822_getn(const zbra_d822 *r, const char *key, size_t idx);

/*
 * Look up an RPM .spec style indexed field: key="Source", idx=2 matches the
 * field named "Source2". Needed because zbra_d822_getn compares whole keys.
 */
const char *zbra_d822_get_idx(const zbra_d822 *r, const char *key, size_t idx);

/*
 * Split a comma-separated field value into its elements, e.g. the Depends
 * field of a binary package. Elements are trimmed of surrounding
 * whitespace. Alternatives separated by '|' are NOT expanded here; use
 * zbra_dep_parse for that.
 *
 * Returns 0 on success and stores a heap array of heap strings in *out
 * with a count in *n, to be released with zbra_d822_strlist_free.
 * Returns -1 on allocation failure.
 */
int zbra_d822_split(const char *value, char ***out, size_t *n);

void zbra_d822_strlist_free(char **list, size_t n);

/*
 * Parse one dependency expression, including alternatives:
 *
 *   "libc6 (>= 2.38)"    -> name="libc6", constraints={">= 2.38"}
 *   "python3:any"        -> name="python3", qual="any"
 *   "foo | bar"          -> name="foo", alt="bar"
 *
 * Returns 0 on success, -1 on allocation failure or a malformed
 * expression. Release with zbra_dep_free.
 *
 * Only the FIRST alternative is split out. An expression with N
 * alternatives is N separate satisfiable requirements, not one, so callers
 * that care about completeness should either split on '|' themselves or
 * rely on zbra_dep_list_parse, which expands each alternative in turn.
 */
typedef struct {
    char           *name;
    char           *qual;           /* "any" / "native", or NULL */
    char          **constraints;
    size_t          n;
    char           *alt;            /* text after '|', or NULL */
} zbra_dep_full;

int zbra_dep_parse(const char *text, zbra_dep_full *out);
void zbra_dep_free(zbra_dep_full *d);

/*
 * Parse a comma-separated Depends/Requires field into individual
 * dependency expressions. Each entry is a zbra_dep_full.
 */
int zbra_dep_list_parse(const char *value, zbra_dep_full **out, size_t *n);
void zbra_dep_list_free(zbra_dep_full *list, size_t n);

/*
 * Render a dependency back to canonical text, e.g.
 * "libc6 (>= 2.38) | zlib1g". Used for error messages and for the
 * dependency list stored in the package database.
 */
char *zbra_dep_format(const zbra_dep_full *d);

/* A growable string buffer, used by the formatters and by the commit log. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} zbra_sb;

void zbra_sb_init(zbra_sb *sb);
void zbra_sb_free(zbra_sb *sb);
int  zbra_sb_add(zbra_sb *sb, const char *s);
int  zbra_sb_addf(zbra_sb *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_DEB822_H */
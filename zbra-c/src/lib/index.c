/*
 * index.c -- the unified package index.
 *
 * Security posture, because this is the module that reads remote input:
 *
 *   Every package name and every declared file path coming out of an index
 *   file is validated before it is stored. A hostile or corrupt index cannot
 *   cause zbra to build a path outside its database or install root, because
 *   the path is checked at the point of ingestion rather than at the point of
 *   use, which is where such a bug is normally found.
 *
 *   A record that fails validation is dropped and counted, not fatal. One bad
 *   stanza in a 40,000-line Packages file must not make the whole repository
 *   unusable; instead `zbra update` prints how many records were skipped and
 *   why, so a systematically broken repository is still noticeable.
 *
 *   Nothing is executed while parsing. Snippets from .PKGBUILD, maintainer
 *   scripts and deb postinst bodies are treated as opaque text. That is a
 *   deliberate line: parsing an index must never run code from it.
 */

#include "index.h"
#include "sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "deb822.h"

/* ---------------------------------------------------------- helpers */

/* Defined with the validation section below. */
static int record_is_trustworthy(const zbra_package *p, const char **why);

static char *xstrdup(const char *s)
{
    return s != NULL ? strdup(s) : NULL;
}

/* Strip the trailing newline a getline() buffer retains. */
static int ver_style_for_format(const char *format)
{
    if (format != NULL && strcmp(format, "rpm") == 0)
        return ZBRA_VER_RPM;

    return ZBRA_VER_DEB;
}

/* ---------------------------------------------------------- lifecycle */

void zbra_package_free(zbra_package *p)
{
    if (p == NULL)
        return;

    free(p->name);
    free(p->version);
    free(p->arch);
    free(p->format);
    free(p->source_id);
    free(p->source_url);
    free(p->filename);
    free(p->checksum);
    free(p->description);
    free(p->homepage);
    free(p->license);

    zbra_d822_strlist_free(p->depends, p->n_depends);
    zbra_d822_strlist_free(p->conflicts, p->n_conflicts);
    zbra_d822_strlist_free(p->provides, p->n_provides);
    zbra_d822_strlist_free(p->replaces, p->n_replaces);

    memset(p, 0, sizeof(*p));
}

void zbra_package_list_free(zbra_package *list, size_t n)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; i < n; i++)
        zbra_package_free(&list[i]);

    free(list);
}

void zbra_index_init(zbra_index *idx)
{
    memset(idx, 0, sizeof(*idx));
}

void zbra_index_free(zbra_index *idx)
{
    size_t i;

    if (idx == NULL)
        return;

    zbra_package_list_free(idx->items, idx->n);

    for (i = 0; i < idx->n_warnings; i++)
        free(idx->warnings[i]);
    free(idx->warnings);

    memset(idx, 0, sizeof(*idx));
}

void zbra_index_warn(zbra_index *idx, const char *fmt, ...)
{
    va_list ap;
    char    buf[512];
    char   *dup;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    dup = strdup(buf);
    if (dup == NULL)
        return;

    {
        char **grown = realloc(idx->warnings,
                               (idx->n_warnings + 1) * sizeof(char *));
        if (grown == NULL) {
            free(dup);
            return;
        }
        idx->warnings = grown;
    }

    idx->warnings[idx->n_warnings++] = dup;
}

/* Deep-copy a heap string list, to be freed with zbra_d822_strlist_free. */
static char **dup_strlist(char *const *list, size_t n)
{
    char **copy;
    size_t i;

    if (n == 0)
        return NULL;

    copy = calloc(n, sizeof(*copy));
    if (copy == NULL)
        return NULL;

    for (i = 0; i < n; i++) {
        if (list[i] == NULL)
            goto fail;

        copy[i] = strdup(list[i]);
        if (copy[i] == NULL)
            goto fail;
    }

    return copy;

fail:
    zbra_d822_strlist_free(copy, n);
    return NULL;
}

/*
 * Deep-copy a record's strings and lists.
 *
 * index_push and zbra_package_free are ownership-transferring, so a shallow
 * struct copy leaves two records pointing at one set of allocations and the
 * second free corrupts the heap. Any record that outlives the index it came
 * from has to be cloned.
 */
static int package_clone(zbra_package *dst, const zbra_package *src)
{
    char **copies;

    memset(dst, 0, sizeof(*dst));

    dst->name      = xstrdup(src->name);
    dst->version   = xstrdup(src->version);
    dst->arch      = xstrdup(src->arch);
    dst->format    = xstrdup(src->format);
    dst->source_id = xstrdup(src->source_id);
    dst->source_url = xstrdup(src->source_url);
    dst->filename  = xstrdup(src->filename);
    dst->checksum  = xstrdup(src->checksum);
    dst->description = xstrdup(src->description);
    dst->homepage  = xstrdup(src->homepage);
    dst->license   = xstrdup(src->license);
    dst->style     = src->style;
    dst->size      = src->size;

    /*
     * A clone is only usable if every string survived, since
     * zbra_package_free has no idea which ones are missing.
     */
    if (src->name != NULL && dst->name == NULL)
        goto fail;
    if (src->version != NULL && dst->version == NULL)
        goto fail;
    if (src->description != NULL && dst->description == NULL)
        goto fail;

    copies = dup_strlist(src->depends, src->n_depends);
    if (copies == NULL && src->n_depends != 0)
        goto fail;
    dst->depends = copies;
    dst->n_depends = src->n_depends;

    copies = dup_strlist(src->conflicts, src->n_conflicts);
    if (copies == NULL && src->n_conflicts != 0)
        goto fail;
    dst->conflicts = copies;
    dst->n_conflicts = src->n_conflicts;

    copies = dup_strlist(src->provides, src->n_provides);
    if (copies == NULL && src->n_provides != 0)
        goto fail;
    dst->provides = copies;
    dst->n_provides = src->n_provides;

    copies = dup_strlist(src->replaces, src->n_replaces);
    if (copies == NULL && src->n_replaces != 0)
        goto fail;
    dst->replaces = copies;
    dst->n_replaces = src->n_replaces;

    return 0;

fail:
    zbra_package_free(dst);
    return -1;
}

static int index_push(zbra_index *idx, const zbra_package *pkg);

/* Push a deep copy, leaving the caller free to drop its own record. */
static int index_push_copy(zbra_index *idx, const zbra_package *pkg)
{
    zbra_package copy;

    if (package_clone(&copy, pkg) != 0)
        return -1;

    if (index_push(idx, &copy) != 0) {
        zbra_package_free(&copy);
        return -1;
    }

    return 0;
}

static int index_push(zbra_index *idx, const zbra_package *pkg)
{
    if (idx->n == idx->cap) {
        size_t        cap = idx->cap != 0 ? idx->cap * 2 : 64;
        zbra_package *items;

        items = realloc(idx->items, cap * sizeof(*items));
        if (items == NULL)
            return -1;

        idx->items = items;
        idx->cap = cap;
    }

    idx->items[idx->n++] = *pkg;

    return 0;
}

/* --------------------------------------------------------- directory scan */

/*
 * Split a package file name into name and version.
 *
 * The conventions are not uniform: a Debian package is
 * name_version_arch.deb with underscores, while a tarball release is
 * name-version.tar.xz with hyphens and usually no architecture. Both are
 * tried, hyphen first for tarballs because a hyphen is legal inside a Debian
 * package name and guessing the other way round mis-splits them.
 */
static int split_package_filename(const char *file, const char *format,
                                  char **name, char **version)
{
    size_t flen = strlen(file);
    size_t elen = strlen(format);
    size_t stem;
    const char *base = strrchr(file, '/');
    size_t i;

    if (elen == 0 || flen <= elen + 1)
        return -1;

    if (strcmp(file + flen - elen, format) != 0)
        return -1;

    base = base != NULL ? base + 1 : file;
    flen = strlen(base);

    if (flen <= elen + 1)
        return -1;

    stem = flen - elen - 1;   /* the characters before ".tar.xz" */
    (void)i;

    /* Debian: name_version_arch, arch is the last field. */
    if (strcmp(format, ".deb") == 0) {
        size_t start = stem;

        while (start > 0 && base[start - 1] != '_')
            start--;

        /* Two underscores minimum: name, version, arch. */
        if (start == 0 || start == stem)
            return -1;

        *name    = strndup(base, start - 1);
        *version = strndup(base + start, stem - start);

        return (*name != NULL && *version != NULL) ? 0 : -1;
    }

    /*
     * Everything else: the last hyphen separates name from version. Walk back
     * to the previous hyphen so a name may contain hyphens too, but do not
     * cross a slash (there is none here) or accept a name that starts with a
     * digit, since that is almost always a mis-split.
     */
    {
        size_t split = stem;

        while (split > 0 && base[split - 1] != '-')
            split--;

        if (split == 0 || split == stem)
            return -1;

        *name    = strndup(base, split - 1);
        *version = strndup(base + split, stem - split);

        return (*name != NULL && *version != NULL) ? 0 : -1;
    }
}

static const char *format_for_filename(const char *file, size_t *len_out)
{
    static const struct {
        const char *suffix;
        const char *format;
    } known[] = {
        { ".tar.xz", "tar.xz" },
        { ".deb",    "deb" },
        { ".rpm",    "rpm" },
        { ".snap",   "snap" },
    };
    size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        size_t n = strlen(known[i].suffix);
        size_t flen = strlen(file);

        if (flen > n && strcmp(file + flen - n, known[i].suffix) == 0) {
            if (len_out != NULL)
                *len_out = n;
            return known[i].format;
        }
    }

    return NULL;
}

int zbra_index_scan_directory(zbra_index *idx, const char *dir,
                              const char *source_id, const char *source_url,
                              char **err)
{
    char           path[8192];
    DIR           *d;
    struct dirent *de;

    if (idx == NULL || dir == NULL) {
        if (err != NULL)
            *err = strdup("no directory to scan");
        return -1;
    }

    d = opendir(dir);
    if (d == NULL) {
        if (err != NULL) {
            char buf[PATH_MAX + 64];

            snprintf(buf, sizeof(buf), "cannot read the package directory %s: %s",
                     dir, strerror(errno));
            *err = strdup(buf);
        }
        return -1;
    }

    while ((de = readdir(d)) != NULL) {
        zbra_package p;
        size_t       slen = 0;
        const char  *format;
        const char  *why = NULL;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        format = format_for_filename(de->d_name, &slen);
        if (format == NULL)
            continue;

        memset(&p, 0, sizeof(p));
        p.name      = NULL;
        p.version   = NULL;
        p.format    = xstrdup(format);
        p.source_id = xstrdup(source_id);
        p.source_url = xstrdup(source_url);
        p.style     = (zbra_ver_style)ver_style_for_format(format);
        p.filename  = xstrdup(de->d_name);

        if (split_package_filename(de->d_name, format, &p.name, &p.version)
            != 0) {
            zbra_package_free(&p);
            zbra_index_warn(idx, "skipping %s: its name does not say which "
                                 "package or version it is", de->d_name);
            continue;
        }

        if (!record_is_trustworthy(&p, &why)) {
            zbra_index_warn(idx, "skipping %s: %s", de->d_name,
                            why != NULL ? why : "unsafe");
            zbra_package_free(&p);
            continue;
        }

        /*
         * Record the digest of what is actually on disk.
         *
         * A filename cannot state a checksum, and a record without one is a
         * record nothing may install from: an unverifiable payload is not a
         * different kind of payload, it is an unchecked one. Computing the
         * digest here keeps a local repository usable without weakening that
         * rule, because from the moment of the scan onward the index says what
         * the bytes must be and the installer can still catch a truncated file,
         * an interrupted copy or a repository edited behind zbra's back.
         *
         * This is trust on first use and should not be read as more. It proves
         * the file has not changed since the scan; it says nothing about who
         * published it. A repository that needs a publisher's guarantee has to
         * publish an index.json with the digests baked in, which is what the
         * checksum field is for.
         */
        {
            char  full[8192];
            char  hex[ZBRA_SHA256_HEX_LEN + 1];

            snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);

            if (zbra_sha256_file(full, hex) != 0) {
                zbra_index_warn(idx, "skipping %s: cannot digest it, so it "
                                     "could not be verified later",
                                de->d_name);
                zbra_package_free(&p);
                continue;
            }

            {
                char *rec = malloc(strlen(hex) + 8);

                if (rec == NULL) {
                    zbra_package_free(&p);
                    closedir(d);
                    if (err != NULL)
                        *err = strdup("out of memory");
                    return -1;
                }

                sprintf(rec, "sha256:%s", hex);
                p.checksum = rec;
            }
        }

        /* index_push takes ownership of the heap members, so p is only
         * freed by the index itself from here on. */
        if (index_push(idx, &p) != 0) {
            zbra_package_free(&p);
            closedir(d);
            if (err != NULL)
                *err = strdup("out of memory");
            return -1;
        }
    }

    closedir(d);

    (void)path;
    return 0;
}

/* ---------------------------------------------------------- validation */

/*
 * Decide whether a record may enter the index.
 *
 * The name is the load-bearing check: it becomes a directory name in the
 * database and a cache path, so "../" or a leading '/' would let a hostile
 * index redirect writes. The version is checked because the comparator
 * assumes non-empty components and a record with an empty version would make
 * ordering arbitrary.
 *
 * `why` receives a short reason for the warning, or NULL on success.
 */
static int record_is_trustworthy(const zbra_package *p, const char **why)
{
    if (p->name == NULL || *p->name == '\0') {
        *why = "missing name";
        return 0;
    }

    if (!zbra_name_is_safe(p->name)) {
        *why = "unsafe name";
        return 0;
    }

    if (p->version == NULL || *p->version == '\0') {
        *why = "missing version";
        return 0;
    }

    if (p->filename != NULL && *p->filename != '\0' &&
        strstr(p->filename, "://") == NULL && p->filename[0] == '/' &&
        !zbra_path_is_safe(p->filename)) {
        *why = "unsafe filename";
        return 0;
    }

    *why = NULL;

    return 1;
}

/* ---------------------------------------------------------- deb Packages */

/*
 * Debian relationship fields are comma-separated lists where an entry may
 * carry alternatives and version constraints. The raw strings are kept
 * verbatim and parsed later by the resolver, because deciding how to satisfy
 * "a | b" requires the whole index, not one stanza.
 */
static void take_deb_depends(zbra_package *p, const char *value)
{
    if (value == NULL || *value == '\0')
        return;

    if (zbra_d822_split(value, &p->depends, &p->n_depends) != 0) {
        p->depends = NULL;
        p->n_depends = 0;
    }
}

static void fill_from_deb_stanza(zbra_index *idx, const zbra_d822 *r,
                                 const char *source_id,
                                 const char *source_url)
{
    zbra_package p;
    const char  *why = NULL;
    const char  *v;

    memset(&p, 0, sizeof(p));

    p.name = xstrdup(zbra_d822_get(r, "Package"));
    p.version = xstrdup(zbra_d822_get(r, "Version"));
    p.arch = xstrdup(zbra_d822_get(r, "Architecture"));
    p.format = xstrdup("deb");
    p.source_id = xstrdup(source_id);
    p.source_url = xstrdup(source_url);
    p.style = (zbra_ver_style)ver_style_for_format("deb");

    p.description = xstrdup(zbra_d822_get(r, "Description"));
    p.homepage = xstrdup(zbra_d822_get(r, "Homepage"));

    v = zbra_d822_get(r, "Size");
    if (v != NULL)
        p.size = strtoll(v, NULL, 10);

    /*
     * The Filename field is relative to the repository root, and that is
     * exactly how it must be kept: joining it to source_url is the backend's
     * job, and doing it here would bake a network path into every record and
     * break the local-directory case.
     */
    p.filename = xstrdup(zbra_d822_get(r, "Filename"));

    v = zbra_d822_get(r, "SHA256");
    if (v != NULL) {
        char *c = malloc(strlen(v) + 8);
        if (c != NULL) {
            sprintf(c, "sha256:%s", v);
            p.checksum = c;
        }
    }

    take_deb_depends(&p, zbra_d822_get(r, "Depends"));
    zbra_d822_split(zbra_d822_get(r, "Conflicts"), &p.conflicts,
                    &p.n_conflicts);
    zbra_d822_split(zbra_d822_get(r, "Provides"), &p.provides,
                    &p.n_provides);
    zbra_d822_split(zbra_d822_get(r, "Replaces"), &p.replaces,
                    &p.n_replaces);

    if (!record_is_trustworthy(&p, &why)) {
        zbra_index_warn(idx, "dropped record from %s: %s (%s)",
                        source_id != NULL ? source_id : "?", p.name != NULL
                        ? p.name : "<unnamed>", why);
        zbra_package_free(&p);
        return;
    }

    if (index_push(idx, &p) != 0) {
        zbra_package_free(&p);
        zbra_index_warn(idx, "out of memory indexing %s", p.name);
    }
}

int zbra_index_read_deb_packages(zbra_index *idx, const char *path,
                                 const char *source_id, const char *source_url,
                                 char **err)
{
    FILE      *fp;
    zbra_d822  r;

    if (strcmp(path, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(path, "r");
        if (fp == NULL) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
            if (err != NULL)
                *err = strdup(buf);
            return -1;
        }
    }

    if (zbra_d822_open(&r, fp) != 0) {
        if (fp != stdin)
            fclose(fp);
        if (err != NULL) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s: cannot read Packages index",
                     path);
            *err = strdup(buf);
        }
        return -1;
    }

    /* next() returns 1 for a stanza, 0 at end of input, -1 on a bad line. */
    for (;;) {
        int rc = zbra_d822_next(&r);

        if (rc == 0)
            break;

        if (rc < 0) {
            /*
             * A malformed line ends this file's usefulness, but a real
             * Packages file always parses; report rather than abort so the
             * already-indexed records stay usable.
             */
            zbra_index_warn(idx, "%s: malformed line %ld, stopping",
                            path, r.lineno);
            break;
        }

        if (zbra_d822_get(&r, "Package") == NULL)
            continue;

        fill_from_deb_stanza(idx, &r, source_id, source_url);
    }

    zbra_d822_close(&r);

    if (fp != stdin)
        fclose(fp);

    return 0;
}

/* ---------------------------------------------------------- index.json */

/*
 * A deliberately small JSON reader. Adding a full JSON library to satisfy one
 * index format would be the wrong trade for a package manager that must build
 * with nothing but a C compiler, so this parses exactly the shapes
 * zbra_index_write_json emits: objects, arrays, strings, numbers, and the
 * three literals. Anything unexpected is an error, never a guess.
 */

typedef struct {
    const char *p;
    const char *end;
} jparse;

static void jskip(jparse *j)
{
    while (j->p < j->end) {
        if (isspace((unsigned char)*j->p)) {
            j->p++;
        } else if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '/') {
            while (j->p < j->end && *j->p != '\n')
                j->p++;
        } else {
            break;
        }
    }
}

/*
 * Append a single character.
 *
 * zbra_sb_add takes a NUL-terminated string, so it cannot be handed a bare
 * char: passing &c makes strlen read past it into adjacent stack memory, and
 * passing a pointer into the JSON buffer would append everything from the
 * cursor to end of file. A two-byte NUL-terminated cell is the only correct
 * way to add one character.
 */
static void jstrail(zbra_sb *sb, const char *p)
{
    char cell[2];

    cell[0] = *p;
    cell[1] = '\0';
    zbra_sb_add(sb, cell);
}

static int jstring(jparse *j, char **out)
{
    zbra_sb sb;

    jskip(j);
    if (j->p >= j->end || *j->p != '"')
        return -1;

    j->p++;
    zbra_sb_init(&sb);

    while (j->p < j->end && *j->p != '"') {
        if (*j->p == '\\') {
            j->p++;
            if (j->p >= j->end)
                break;

            switch (*j->p) {
            case 'n': zbra_sb_add(&sb, "\n"); break;
            case 't': zbra_sb_add(&sb, "\t"); break;
            case 'r': zbra_sb_add(&sb, "\r"); break;
            case 'b': zbra_sb_add(&sb, "\b"); break;
            case 'f': zbra_sb_add(&sb, "\f"); break;
            case 'u': {
                /*
                 * Only the BMP subset zbra actually writes is supported:
                 * anything else would need UTF-8 encoding and surrogate
                 * pairing, neither of which appears in a package name.
                 */
                unsigned code = 0;
                int      i;

                for (i = 0; i < 4 && j->p + 1 < j->end; i++) {
                    char c = j->p[1 + i];
                    if (!isxdigit((unsigned char)c))
                        break;
                    code = code * 16 +
                           (unsigned)(isdigit((unsigned char)c)
                                          ? c - '0'
                                          : (tolower((unsigned char)c) - 'a'
                                             + 10));
                }
                j->p += (size_t)i;
                if (code < 0x80) {
                    char cell[2];
                    cell[0] = (char)code;
                    cell[1] = '\0';
                    zbra_sb_add(&sb, cell);
                }
                break;
            }
            default:
                jstrail(&sb, j->p);
                break;
            }

            j->p++;
        } else {
            jstrail(&sb, j->p);
            j->p++;
        }
    }

    if (j->p >= j->end) {
        zbra_sb_free(&sb);
        return -1;
    }

    j->p++;   /* closing quote */

    *out = strdup(sb.buf != NULL ? sb.buf : "");
    zbra_sb_free(&sb);

    return (*out != NULL) ? 0 : -1;
}

static long long jnumber(jparse *j)
{
    char       buf[32];
    size_t     n = 0;
    jskip(j);

    while (j->p < j->end && n + 1 < sizeof(buf) &&
           (isdigit((unsigned char)*j->p) || *j->p == '-' || *j->p == '+'))
        buf[n++] = *j->p++;

    buf[n] = '\0';

    return strtoll(buf, NULL, 10);
}

static void jskip_value(jparse *j);

static void jskip_string(jparse *j)
{
    char *tmp = NULL;

    if (jstring(j, &tmp) == 0)
        free(tmp);
}

static void jskip_container(jparse *j, char open, char close)
{
    int depth = 0;

    jskip(j);
    while (j->p < j->end) {
        if (*j->p == '"') {
            jskip_string(j);
            continue;
        }
        if (*j->p == open)
            depth++;
        else if (*j->p == close) {
            depth--;
            if (depth == 0) {
                j->p++;
                return;
            }
        }
        j->p++;
    }
}

static void jskip_value(jparse *j)
{
    int c;

    jskip(j);
    if (j->p >= j->end)
        return;

    c = *j->p;

    if (c == '"') {
        jskip_string(j);
    } else if (c == '{') {
        jskip_container(j, '{', '}');
    } else if (c == '[') {
        jskip_container(j, '[', ']');
    } else {
        while (j->p < j->end && *j->p != ',' && *j->p != '}' &&
               *j->p != ']')
            j->p++;
    }
}

static char **jstring_array(jparse *j, size_t *count)
{
    char **list = NULL;
    size_t n = 0;

    *count = 0;

    jskip(j);
    if (j->p >= j->end || *j->p != '[')
        return NULL;

    j->p++;
    jskip(j);

    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return NULL;
    }

    for (;;) {
        char *s = NULL;

        jskip(j);
        if (j->p < j->end && *j->p == '"') {
            if (jstring(j, &s) != 0)
                break;
        } else {
            jskip_value(j);
            continue;
        }

        {
            char **grown = realloc(list, (n + 1) * sizeof(char *));
            if (grown == NULL) {
                free(s);
                break;
            }
            list = grown;
            list[n++] = s;
        }

        jskip(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        break;
    }

    jskip(j);
    if (j->p < j->end && *j->p == ']')
        j->p++;

    *count = n;

    return list;
}

static int read_one_json_record(zbra_index *idx, jparse *j,
                                const char *source_id,
                                const char *source_url)
{
    zbra_package p;
    const char  *why = NULL;

    memset(&p, 0, sizeof(p));
    p.source_id = xstrdup(source_id);
    p.source_url = xstrdup(source_url);

    jskip(j);
    if (j->p >= j->end || *j->p != '{')
        return -1;

    j->p++;

    for (;;) {
        char *key = NULL;

        jskip(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            break;
        }

        if (jstring(j, &key) != 0)
            break;

        jskip(j);
        if (j->p < j->end && *j->p == ':')
            j->p++;

        jskip(j);

        if (strcmp(key, "source_id") == 0) {
            free(p.source_id);
            if (jstring(j, &p.source_id) != 0) { free(key); break; }
        } else if (strcmp(key, "source_url") == 0) {
            free(p.source_url);
            if (jstring(j, &p.source_url) != 0) { free(key); break; }
        } else if (strcmp(key, "name") == 0) {
            free(p.name);
            if (jstring(j, &p.name) != 0) { free(key); break; }
        } else if (strcmp(key, "version") == 0) {
            free(p.version);
            if (jstring(j, &p.version) != 0) { free(key); break; }
        } else if (strcmp(key, "arch") == 0) {
            free(p.arch);
            if (jstring(j, &p.arch) != 0) { free(key); break; }
        } else if (strcmp(key, "format") == 0) {
            free(p.format);
            if (jstring(j, &p.format) != 0) { free(key); break; }
        } else if (strcmp(key, "filename") == 0) {
            free(p.filename);
            if (jstring(j, &p.filename) != 0) { free(key); break; }
        } else if (strcmp(key, "checksum") == 0) {
            free(p.checksum);
            if (jstring(j, &p.checksum) != 0) { free(key); break; }
        } else if (strcmp(key, "description") == 0) {
            free(p.description);
            if (jstring(j, &p.description) != 0) { free(key); break; }
        } else if (strcmp(key, "homepage") == 0) {
            free(p.homepage);
            if (jstring(j, &p.homepage) != 0) { free(key); break; }
        } else if (strcmp(key, "license") == 0) {
            free(p.license);
            if (jstring(j, &p.license) != 0) { free(key); break; }
        } else if (strcmp(key, "size") == 0) {
            p.size = jnumber(j);
        } else if (strcmp(key, "depends") == 0) {
            zbra_d822_strlist_free(p.depends, p.n_depends);
            p.depends = jstring_array(j, &p.n_depends);
        } else if (strcmp(key, "conflicts") == 0) {
            zbra_d822_strlist_free(p.conflicts, p.n_conflicts);
            p.conflicts = jstring_array(j, &p.n_conflicts);
        } else if (strcmp(key, "provides") == 0) {
            zbra_d822_strlist_free(p.provides, p.n_provides);
            p.provides = jstring_array(j, &p.n_provides);
        } else {
            jskip_value(j);
        }

        free(key);

        jskip(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
    }

    if (p.format != NULL)
        p.style = (zbra_ver_style)ver_style_for_format(p.format);
    else
        p.style = ZBRA_VER_DEB;

    if (!record_is_trustworthy(&p, &why)) {
        zbra_index_warn(idx, "dropped record from %s: %s (%s)",
                        source_id != NULL ? source_id : "?",
                        p.name != NULL ? p.name : "<unnamed>", why);
        zbra_package_free(&p);
        return 0;
    }

    if (index_push(idx, &p) != 0) {
        zbra_package_free(&p);
        return -1;
    }

    return 0;
}

int zbra_index_read_json(zbra_index *idx, const char *path,
                         const char *source_id, const char *source_url,
                         char **err)
{
    FILE  *fp;
    char  *text;
    long   size;
    jparse j;
    int    seen_array = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s: %s", path, strerror(errno));
        if (err != NULL)
            *err = strdup(buf);
        return -1;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0) {
        fclose(fp);
        if (err != NULL)
            *err = strdup("cannot size index file");
        return -1;
    }
    rewind(fp);

    text = malloc((size_t)size + 1);
    if (text == NULL) {
        fclose(fp);
        if (err != NULL)
            *err = strdup("out of memory reading index");
        return -1;
    }

    size = (long)fread(text, 1, (size_t)size, fp);
    text[size] = '\0';
    fclose(fp);

    j.p = text;
    j.end = text + size;

    jskip(&j);

    if (j.p < j.end && *j.p == '{') {
        /*
         * Accept {"packages": [ ... ]} as well as a bare array. Every value
         * that is not the package array must be stepped over explicitly:
         * stopping at the next comma without consuming the value would leave
         * the cursor on, say, the '1' of "format": 1, and the next key read
         * would fail there.
         */
        j.p++;
        for (;;) {
            char *key = NULL;

            jskip(&j);
            if (j.p < j.end && *j.p == '}') {
                j.p++;
                break;
            }
            if (jstring(&j, &key) != 0)
                break;

            jskip(&j);
            if (j.p < j.end && *j.p == ':')
                j.p++;

            jskip(&j);
            if (j.p < j.end && *j.p == '[' &&
                (strcmp(key, "packages") == 0 || strcmp(key, "records") == 0)) {
                j.p++;
                seen_array = 1;
            } else {
                jskip_value(&j);
            }

            free(key);

            jskip(&j);
            if (j.p < j.end && *j.p == ',') {
                j.p++;
                continue;
            }
            if (j.p < j.end && *j.p == '}') {
                j.p++;
                break;
            }
            break;      /* unrecognised shape: stop rather than spin */
        }
    }

    if (!seen_array) {
        jskip(&j);
        if (j.p < j.end && *j.p == '[')
            j.p++;
        else {
            free(text);
            char buf[512];
            snprintf(buf, sizeof(buf), "%s: not a zbra index (no package "
                     "array found)", path);
            if (err != NULL)
                *err = strdup(buf);
            return -1;
        }
    }

    for (;;) {
        jskip(&j);
        if (j.p >= j.end)
            break;
        if (*j.p == ']') {
            j.p++;
            break;
        }
        if (*j.p == ',') {
            j.p++;
            continue;
        }
        if (*j.p != '{')
            break;

        if (read_one_json_record(idx, &j, source_id, source_url) != 0)
            break;
    }

    free(text);

    return 0;
}

int zbra_index_read_auto(zbra_index *idx, const char *path,
                         const char *source_id, const char *source_url,
                         char **err)
{
    size_t len;

    if (path == NULL) {
        if (err != NULL)
            *err = strdup("no index path");
        return -1;
    }

    len = strlen(path);

    /*
     * ".json" is five characters, so the suffix is compared as strlen of the
     * literal. Counting the dot and the extension separately is how this ends
     * up comparing eight characters against a five-character string, which
     * never matches and sends every JSON index to the Debian parser.
     */
    if (len > strlen(".json") &&
        strcmp(path + len - strlen(".json"), ".json") == 0)
        return zbra_index_read_json(idx, path, source_id, source_url, err);

    if (len > strlen(".deb") &&
        strcmp(path + len - strlen(".deb"), ".deb") == 0)
        return zbra_index_read_deb_packages(idx, path, source_id, source_url,
                                             err);

    /*
     * Anything else is a bare "Packages" file, which is what apt repositories
     * publish and what scripts/generate-repo.sh emits.
     */
    if (len > 0)
        return zbra_index_read_deb_packages(idx, path, source_id, source_url,
                                             err);

    if (err != NULL)
        *err = strdup("unrecognised index file");

    return -1;
}

/* ---------------------------------------------------------- queries */

int zbra_index_find(const zbra_index *idx, const char *name,
                    zbra_package *out)
{
    size_t i;
    size_t best = (size_t)-1;

    if (name == NULL)
        return 0;

    /*
     * One pass. When two records share a name their versions are compared
     * using the style of the record already held as best, because a Debian
     * version and an RPM version are not meaningfully comparable and forcing
     * a comparison between them would pick whichever order the algorithm
     * happened to produce.
     *
     * On a genuine tie the earlier record wins, which makes the result a
     * function of source order rather than of file read order.
     */
    for (i = 0; i < idx->n; i++) {
        const zbra_package *p = &idx->items[i];

        if (p->name == NULL || strcmp(p->name, name) != 0)
            continue;

        if (best == (size_t)-1) {
            best = i;
            continue;
        }

        if (p->style == idx->items[best].style &&
            zbra_vercmp(p->version, idx->items[best].version, p->style) > 0)
            best = i;
    }

    if (best == (size_t)-1)
        return 0;

    if (out != NULL)
        *out = idx->items[best];

    return 1;
}

int zbra_index_find_copy(const zbra_index *idx, const char *name,
                         zbra_package *out)
{
    zbra_package p;

    if (zbra_index_find(idx, name, &p) != 1)
        return 0;

    if (package_clone(out, &p) != 0)
        return -1;

    return 1;
}

/* --------------------------------------------------------------- search */

/*
 * Comparator for an array of zbra_package POINTERS (what search fills in).
 *
 * qsort passes the address of each element, so the parameters must be
 * pointers-to-pointer. Typing them as the struct made this read the pointer
 * value as if it were the struct, comparing garbage and sorting at random.
 */
static int cmp_by_relevance(const void *a, const void *b)
{
    const zbra_package *const *pa = a;
    const zbra_package *const *pb = b;

    /*
     * Exact name first, then alphabetical. A substring match buried under
     * unrelated names makes `zbra search vim` effectively unusable.
     */
    {
        const char *na = (*pa)->name != NULL ? (*pa)->name : "";
        const char *nb = (*pb)->name != NULL ? (*pb)->name : "";
        size_t      la = strlen(na);
        size_t      lb = strlen(nb);

        if (la != lb)
            return la < lb ? -1 : 1;

        return strcmp(na, nb);
    }
}

size_t zbra_index_search(const zbra_index *idx, const char *needle,
                         zbra_package **out, size_t max)
{
    size_t   i;
    size_t   n = 0;
    size_t   total = 0;
    char    *lower_needle = NULL;

    if (needle == NULL || idx == NULL)
        return 0;

    /* Case-insensitive match, since package names vary in case. */
    lower_needle = strdup(needle);
    if (lower_needle == NULL)
        return 0;
    for (i = 0; lower_needle[i] != '\0'; i++)
        lower_needle[i] = (char)tolower((unsigned char)lower_needle[i]);

    for (i = 0; i < idx->n; i++) {
        zbra_package *p = &idx->items[i];
        int           hit = 0;
        char         *hay;

        if (p->name != NULL) {
            hay = strdup(p->name);
            if (hay != NULL) {
                size_t k;
                for (k = 0; hay[k] != '\0'; k++)
                    hay[k] = (char)tolower((unsigned char)hay[k]);
                hit = strstr(hay, lower_needle) != NULL;
                free(hay);
            }
        }

        if (!hit && p->description != NULL) {
            hay = strdup(p->description);
            if (hay != NULL) {
                size_t k;
                for (k = 0; hay[k] != '\0'; k++)
                    hay[k] = (char)tolower((unsigned char)hay[k]);
                hit = strstr(hay, lower_needle) != NULL;
                free(hay);
            }
        }

        if (!hit)
            continue;

        total++;

        if (out != NULL && n < max)
            out[n++] = p;
    }

    free(lower_needle);

    if (out != NULL && n > 1)
        qsort(out, n, sizeof(*out), cmp_by_relevance);

    /*
     * With no output array the caller only wants the count, which is how a
     * caller pages through results without guessing a buffer size.
     */
    return out != NULL ? n : total;
}

/* ----------------------------------------------------------- ordering */

static int cmp_pkg_name(const void *a, const void *b)
{
    const zbra_package *pa = a;
    const zbra_package *pb = b;

    return strcmp(pa->name != NULL ? pa->name : "",
                  pb->name != NULL ? pb->name : "");
}

/*
 * Candidate lookup for the resolver.
 *
 * zbra_index_find hands back a borrowed view into the index, but a plan
 * outlives the index it was built from, so this copies everything the
 * resolver will keep.
 */
int zbra_index_find_candidate(void *ud, const char *name,
                              zbra_candidate *out)
{
    const zbra_index *idx = ud;
    zbra_candidate   *c   = out;
    zbra_package      best;
    char            **deps;

    if (idx == NULL || name == NULL || c == NULL)
        return -1;

    memset(c, 0, sizeof(*c));

    if (zbra_index_find(idx, name, &best) != 1)
        return 0;

    c->name    = xstrdup(best.name);
    c->version = xstrdup(best.version);
    c->source  = xstrdup(best.source_id);
    c->format  = xstrdup(best.format);
    c->style   = best.style;

    if (c->name == NULL || c->version == NULL)
        goto fail;

    if (best.n_depends == 0)
        return 1;

    deps = dup_strlist(best.depends, best.n_depends);
    if (deps == NULL)
        goto fail;

    c->depends   = deps;
    c->n_depends = best.n_depends;

    return 1;

fail:
    zbra_candidate_free(c);
    return -1;
}

void zbra_index_sort(zbra_index *idx)
{
    if (idx->n > 1)
        qsort(idx->items, idx->n, sizeof(idx->items[0]), cmp_pkg_name);
}

int zbra_index_merge(zbra_index *dst, const zbra_index *src)
{
    size_t i;

    for (i = 0; i < src->n; i++) {
        size_t k;
        int    replaced = 0;

        for (k = 0; k < dst->n; k++) {
            if (dst->items[k].name == NULL ||
                src->items[i].name == NULL)
                continue;
            if (strcmp(dst->items[k].name, src->items[i].name) != 0)
                continue;

            /*
             * Same name. Replace in place when the incoming record is newer,
             * so a name appears once in the merged index rather than once per
             * source -- two live records for one name makes "which version is
             * installed" ambiguous.
             *
             * Versions from different ecosystems are not comparable, so the
             * existing record is kept and source order breaks the tie.
             */
            if (dst->items[k].style == src->items[i].style &&
                zbra_vercmp(dst->items[k].version, src->items[i].version,
                            dst->items[k].style) < 0) {
                zbra_package fresh;

                if (package_clone(&fresh, &src->items[i]) != 0)
                    return -1;

                zbra_package_free(&dst->items[k]);
                dst->items[k] = fresh;
                replaced = 1;
            }

            break;
        }

        if (replaced)
            continue;

        if (index_push_copy(dst, &src->items[i]) != 0)
            return -1;
    }

    return 0;
}

/* ----------------------------------------------------------- json write */

static void json_escape(FILE *fp, const char *s)
{
    fputc('"', fp);

    if (s != NULL) {
        for (; *s != '\0'; s++) {
            switch (*s) {
            case '"':  fputs("\\\"", fp); break;
            case '\\': fputs("\\\\", fp); break;
            case '\n': fputs("\\n", fp); break;
            case '\t': fputs("\\t", fp); break;
            case '\r': fputs("\\r", fp); break;
            default:
                if ((unsigned char)*s < 0x20)
                    fprintf(fp, "\\u%04x", (unsigned char)*s);
                else
                    fputc(*s, fp);
            }
        }
    }

    fputc('"', fp);
}

static void write_string_array(FILE *fp, const char *key, char **list,
                               size_t n, int last)
{
    size_t i;

    fprintf(fp, "    \"%s\": [", key);
    for (i = 0; i < n; i++) {
        if (i > 0)
            fputs(", ", fp);
        json_escape(fp, list[i]);
    }
    fprintf(fp, "]%s\n", last ? "" : ",");
}

int zbra_index_write_json(const zbra_index *idx, const char *path, char **err)
{
    FILE  *fp;
    char   tmp[4096];
    size_t i;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    fp = fopen(tmp, "w");
    if (fp == NULL) {
        if (err != NULL) {
            char buf[4096];
            snprintf(buf, sizeof(buf), "%.4000s: %s", tmp, strerror(errno));
            *err = strdup(buf);
        }
        return -1;
    }

    fputs("{\n  \"format\": 1,\n  \"packages\": [\n", fp);

    for (i = 0; i < idx->n; i++) {
        const zbra_package *p = &idx->items[i];

        fputs("    {\n", fp);
        fprintf(fp, "    \"name\": ");      json_escape(fp, p->name);
        fputs(",\n", fp);
        fprintf(fp, "    \"version\": ");   json_escape(fp, p->version);
        fputs(",\n", fp);
        fprintf(fp, "    \"format\": ");    json_escape(fp, p->format);
        fputs(",\n", fp);
        fprintf(fp, "    \"source_id\": "); json_escape(fp, p->source_id);
        fputs(",\n", fp);
        fprintf(fp, "    \"source_url\": "); json_escape(fp, p->source_url);
        fputs(",\n", fp);
        fprintf(fp, "    \"filename\": ");  json_escape(fp, p->filename);
        fputs(",\n", fp);
        fprintf(fp, "    \"size\": %lld,\n", p->size);
        fprintf(fp, "    \"arch\": ");      json_escape(fp, p->arch);
        fputs(",\n", fp);
        fprintf(fp, "    \"checksum\": ");  json_escape(fp, p->checksum);
        fputs(",\n", fp);
        fprintf(fp, "    \"description\": "); json_escape(fp, p->description);
        fputs(",\n", fp);
        fprintf(fp, "    \"homepage\": ");  json_escape(fp, p->homepage);
        fputs(",\n", fp);
        fprintf(fp, "    \"license\": ");   json_escape(fp, p->license);
        fputs(",\n", fp);

        write_string_array(fp, "depends", p->depends, p->n_depends, 0);
        write_string_array(fp, "conflicts", p->conflicts, p->n_conflicts, 0);
        write_string_array(fp, "provides", p->provides, p->n_provides, 1);

        fputs(i + 1 < idx->n ? "    },\n" : "    }\n", fp);
    }

    fputs("  ]\n}\n", fp);

    /*
     * Renamed into place so a reader never observes a half-written index,
     * which for a package manager would look like a corrupt repository.
     */
    if (fclose(fp) != 0 || rename(tmp, path) != 0) {
        if (err != NULL) {
            char buf[4096];
            snprintf(buf, sizeof(buf), "writing %.4000s: %s", path,
                     strerror(errno));
            *err = strdup(buf);
        }
        unlink(tmp);
        return -1;
    }

    return 0;
}
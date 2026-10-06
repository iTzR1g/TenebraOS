/*
 * db.c -- the installed-package database. See db.h for the on-disk layout.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "db.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

void zbra_strlist_free(char **list)
{
    if (list == NULL)
        return;
    free(list);
}

void zbra_strlist_free_owned(char **list)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; list[i] != NULL; i++)
        free(list[i]);
    free(list);
}

/*
 * Package names are used to build paths under the database root, so they are
 * validated strictly. The rule is intentionally narrower than what dpkg or
 * rpm accept: only [A-Za-z0-9+._-], and no leading '-', '.' or '/'. A name
 * beginning with '-' would be read as an option by dpkg when handed to it,
 * and ".." would escape the directory.
 */
int zbra_name_is_safe(const char *name)
{
    size_t i;

    if (name == NULL || *name == '\0')
        return 0;
    if (strlen(name) > 255)
        return 0;

    if (name[0] == '-' || name[0] == '.' || name[0] == '/')
        return 0;

    for (i = 0; name[i] != '\0'; i++) {
        unsigned char c = (unsigned char)name[i];

        if (isalnum(c))
            continue;
        if (c == '+' || c == '-' || c == '_' || c == '.')
            continue;

        return 0;
    }

    return 1;
}

/*
 * Owned paths are relative, slash-separated, with no "." or ".." component
 * and no leading slash. Symlinks that point outside the tree are a separate
 * concern and are checked at commit time, when the payload is in hand.
 */
int zbra_path_is_safe(const char *path)
{
    const char *p = path;
    size_t seg;

    if (path == NULL || *path == '\0')
        return 0;

    /* Absolute paths are rejected: the install root is implied. */
    if (path[0] == '/')
        return 0;

    if (strlen(path) > PATH_MAX)
        return 0;

    while (*p != '\0') {
        seg = 0;

        if (*p == '/') {
            /* No empty components, so no "//". */
            return 0;
        }

        while (p[seg] != '\0' && p[seg] != '/')
            seg++;

        /* "." and ".." components would let a package escape its prefix. */
        if (seg == 1 && p[0] == '.')
            return 0;
        if (seg == 2 && p[0] == '.' && p[1] == '.')
            return 0;

        /* NUL bytes and control characters have no place in a path. */
        {
            size_t i;
            for (i = 0; i < seg; i++) {
                if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] == 0x7f)
                    return 0;
            }
        }

        p += seg;
        if (*p == '/')
            p++;
    }

    return 1;
}

static const char *kind_dir_name(zbra_kind kind)
{
    switch (kind) {
    case ZBRA_KIND_PACKAGE:    return "packages";
    case ZBRA_KIND_DEPENDENCY: return "dependencies";
    case ZBRA_KIND_ISOLATED:   return "isolated";
    }
    return "packages";
}

static const char *kind_meta_name(zbra_kind kind)
{
    switch (kind) {
    case ZBRA_KIND_PACKAGE:    return "package";
    case ZBRA_KIND_DEPENDENCY: return "dependency";
    case ZBRA_KIND_ISOLATED:   return "isolated";
    }
    return "package";
}

/* Build "<root>/<registry>/<name>/<leaf>" into a freshly allocated string. */
static char *db_path(const zbra_db *db, zbra_kind kind, const char *name,
                     const char *leaf)
{
    size_t len;
    char *out;

    len = strlen(db->root) + 1 + strlen(kind_dir_name(kind)) + 1 +
          strlen(name) + 1 + (leaf ? strlen(leaf) : 0) + 1;
    out = malloc(len);
    if (out == NULL)
        return NULL;

    if (leaf != NULL)
        snprintf(out, len, "%s/%s/%s/%s", db->root, kind_dir_name(kind), name,
                 leaf);
    else
        snprintf(out, len, "%s/%s/%s", db->root, kind_dir_name(kind), name);

    return out;
}

/* mkdir -p. Returns 0 if the directory exists afterwards. */
int zbra_mkdir_p(const char *path, mode_t mode)
{
    char *tmp;
    size_t len;
    char *p;
    int rc = 0;

    if (path == NULL || *path == '\0')
        return -1;

    tmp = strdup(path);
    if (tmp == NULL)
        return -1;

    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/')
        tmp[--len] = '\0';

    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/')
            continue;

        *p = '\0';
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
            rc = -1;
            goto out;
        }
        *p = '/';
    }

    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        rc = -1;

out:
    free(tmp);
    return rc;
}

/* Write `content` to `path` via a temporary file and rename, so a crash
 * mid-write cannot leave a half-written record in place. */
static int write_file_atomic(const char *path, const char *content, size_t len)
{
    char tmp[PATH_MAX];
    int fd;
    ssize_t w;
    size_t off = 0;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp%ld", path, (long)getpid())
        >= (int)sizeof(tmp))
        return -1;

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    while (off < len) {
        w = write(fd, content + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return -1;
        }
        off += (size_t)w;
    }

    if (fsync(fd) != 0 || close(fd) != 0) {
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }

    return 0;
}

/* Read a whole file into a NUL-terminated heap buffer. */
static char *read_file(const char *path, size_t *out_len)
{
    int fd;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    ssize_t r;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    for (;;) {
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *nb;

            while (ncap < len + 4096 + 1)
                ncap *= 2;

            nb = realloc(buf, ncap);
            if (nb == NULL) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
            cap = ncap;
        }

        r = read(fd, buf + len, 4096);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return NULL;
        }
        if (r == 0)
            break;
        len += (size_t)r;
    }

    close(fd);

    if (buf == NULL) {
        /* Empty file: still return a valid empty string. */
        buf = calloc(1, 1);
        if (buf == NULL)
            return NULL;
    } else {
        buf[len] = '\0';
    }

    if (out_len != NULL)
        *out_len = len;

    return buf;
}

/* ------------------------------------------------------------------ */
/* String lists                                                       */
/* ------------------------------------------------------------------ */

/* Append a string to a counted, NULL-terminated array. Returns 0/-1. */
static int strlist_push(char ***list, size_t *n, const char *s)
{
    char **nl = realloc(*list, (*n + 2) * sizeof(*nl));
    size_t i;
    int dup = 1;

    if (nl == NULL)
        return -1;

    *list = nl;

    /* Reject duplicates so add_dependent stays idempotent. */
    for (i = 0; i < *n; i++) {
        if (nl[i] != NULL && strcmp(nl[i], s) == 0) {
            dup = 0;
            break;
        }
    }

    if (dup) {
        nl[*n] = strdup(s);
        if (nl[*n] == NULL)
            return -1;
        (*n)++;
    }

    nl[*n] = NULL;
    return 0;
}

/* Split a comma-separated field into a counted list. */
static char **split_commas(const char *value, size_t *n)
{
    char **list = calloc(1, sizeof(*list));
    const char *p;

    *n = 0;
    if (list == NULL || value == NULL)
        return list;

    p = value;
    while (*p != '\0') {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        char *item;

        while (len > 0 && isspace((unsigned char)*p)) {
            p++;
            len--;
        }
        while (len > 0 && isspace((unsigned char)p[len - 1]))
            len--;

        item = strndup(p, len);
        if (item == NULL)
            break;

        if (item[0] != '\0') {
            if (strlist_push(&list, n, item) != 0) {
                free(item);
                break;
            }
        }
        free(item);

        if (comma == NULL)
            break;
        p = comma + 1;
    }

    return list;
}

void zbra_entry_free(zbra_entry *e)
{
    size_t i;

    if (e == NULL)
        return;

    free(e->name);
    free(e->version);
    free(e->source);
    free(e->format);
    free(e->arch);
    free(e->install_root);

    for (i = 0; i < e->n_depends; i++)
        free(e->depends[i]);
    free(e->depends);

    for (i = 0; i < e->n_dependents; i++)
        free(e->dependents[i]);
    free(e->dependents);

    for (i = 0; i < e->n_files; i++)
        free(e->files[i]);
    free(e->files);

    memset(e, 0, sizeof(*e));
}

/* ------------------------------------------------------------------ */
/* Serialisation                                                      */
/* ------------------------------------------------------------------ */

static int entry_serialise(const zbra_entry *e, char **out, size_t *out_len)
{
    size_t cap = 1024;
    size_t len = 0;
    char *buf;
    size_t i;

    buf = malloc(cap);
    if (buf == NULL)
        return -1;

#define APPEND(...)                                                       \
    do {                                                                  \
        int need = snprintf(buf + len, cap - len, __VA_ARGS__);            \
        if (need < 0) { free(buf); return -1; }                            \
        if ((size_t)need >= cap - len) {                                   \
            size_t ncap = cap * 2;                                         \
            char *nb;                                                      \
            while ((size_t)need >= ncap - len)                              \
                ncap *= 2;                                                 \
            nb = realloc(buf, ncap);                                       \
            if (nb == NULL) { free(buf); return -1; }                      \
            buf = nb;                                                      \
            cap = ncap;                                                    \
            need = snprintf(buf + len, cap - len, __VA_ARGS__);             \
            if (need < 0) { free(buf); return -1; }                        \
        }                                                                  \
        len += (size_t)need;                                               \
    } while (0)

    APPEND("name: %s\n", e->name != NULL ? e->name : "");
    APPEND("version: %s\n", e->version != NULL ? e->version : "");
    APPEND("kind: %s\n", kind_meta_name(e->kind));
    if (e->source != NULL)
        APPEND("source: %s\n", e->source);
    if (e->format != NULL)
        APPEND("format: %s\n", e->format);
    if (e->arch != NULL)
        APPEND("arch: %s\n", e->arch);
    if (e->install_root != NULL)
        APPEND("install-root: %s\n", e->install_root);

    if (e->n_depends > 0) {
        APPEND("depends: ");
        for (i = 0; i < e->n_depends; i++)
            APPEND("%s%s", (i > 0) ? ", " : "", e->depends[i]);
        APPEND("\n");
    }

    if (e->n_dependents > 0) {
        APPEND("dependents: ");
        for (i = 0; i < e->n_dependents; i++)
            APPEND("%s%s", (i > 0) ? ", " : "", e->dependents[i]);
        APPEND("\n");
    }

#undef APPEND

    *out = buf;
    *out_len = len;
    return 0;
}

static int entry_parse(zbra_entry *e, const char *text, zbra_kind def_kind)
{
    const char *p = text;

    memset(e, 0, sizeof(*e));
    e->kind = def_kind;
    e->depends = calloc(1, sizeof(*e->depends));
    e->dependents = calloc(1, sizeof(*e->dependents));
    e->files = calloc(1, sizeof(*e->files));

    if (e->depends == NULL || e->dependents == NULL || e->files == NULL) {
        zbra_entry_free(e);
        return -1;
    }

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        const char *colon;
        size_t linelen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);
        char key[64];
        const char *val;

        if (linelen == 0) {
            if (eol == NULL)
                break;
            p = eol + 1;
            continue;
        }

        colon = memchr(p, ':', linelen);
        if (colon == NULL) {
            if (eol == NULL)
                break;
            p = eol + 1;
            continue;
        }

        {
            size_t klen = (size_t)(colon - p);
            if (klen >= sizeof(key))
                klen = sizeof(key) - 1;
            memcpy(key, p, klen);
            key[klen] = '\0';
        }

        /*
         * The value starts after ": " if a space follows the colon, and
         * immediately after the colon otherwise, so hand-written records
         * written as "version:1.0" still parse.
         */
        val = colon + 1;
        if ((size_t)(val - p) < linelen && *val == ' ')
            val++;

        {
            size_t vlen = linelen - (size_t)(val - p);
            while (vlen > 0 && isspace((unsigned char)val[vlen - 1]))
                vlen--;

            if (strcmp(key, "name") == 0) {
                free(e->name);
                e->name = strndup(val, vlen);
            } else if (strcmp(key, "version") == 0) {
                free(e->version);
                e->version = strndup(val, vlen);
            } else if (strcmp(key, "source") == 0) {
                free(e->source);
                e->source = strndup(val, vlen);
            } else if (strcmp(key, "format") == 0) {
                free(e->format);
                e->format = strndup(val, vlen);
            } else if (strcmp(key, "arch") == 0) {
                free(e->arch);
                e->arch = strndup(val, vlen);
            } else if (strcmp(key, "install-root") == 0) {
                free(e->install_root);
                e->install_root = strndup(val, vlen);
            } else if (strcmp(key, "kind") == 0) {
                char k[32];
                size_t kl = (vlen < sizeof(k) - 1) ? vlen : sizeof(k) - 1;
                memcpy(k, val, kl);
                k[kl] = '\0';
                if (strcmp(k, "dependency") == 0)
                    e->kind = ZBRA_KIND_DEPENDENCY;
                else if (strcmp(k, "isolated") == 0)
                    e->kind = ZBRA_KIND_ISOLATED;
                else
                    e->kind = ZBRA_KIND_PACKAGE;
            } else if (strcmp(key, "depends") == 0) {
                char *tmp = strndup(val, vlen);
                if (tmp != NULL) {
                    size_t n = 0;
                    char **lst = split_commas(tmp, &n);
                    size_t i;
                    for (i = 0; i < n; i++)
                        strlist_push(&e->depends, &e->n_depends, lst[i]);
                    for (i = 0; i < n; i++)
                        free(lst[i]);
                    free(lst);
                    free(tmp);
                }
            } else if (strcmp(key, "dependents") == 0) {
                char *tmp = strndup(val, vlen);
                if (tmp != NULL) {
                    size_t n = 0;
                    char **lst = split_commas(tmp, &n);
                    size_t i;
                    for (i = 0; i < n; i++)
                        strlist_push(&e->dependents, &e->n_dependents, lst[i]);
                    for (i = 0; i < n; i++)
                        free(lst[i]);
                    free(lst);
                    free(tmp);
                }
            }
        }

        if (eol == NULL)
            break;
        p = eol + 1;
    }

    return 0;
}

/* Load the "files" sidecar: one owned path per line. */
static int entry_load_files(zbra_db *db, zbra_entry *e, zbra_kind kind)
{
    char *path;
    char *text;
    const char *p;

    path = db_path(db, kind, e->name, "files");
    if (path == NULL)
        return -1;

    text = read_file(path, NULL);
    free(path);

    if (text == NULL)
        return 0;                   /* no file list yet: not an error */

    p = text;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        if (len > 0) {
            char *item;

            while (len > 0 && isspace((unsigned char)p[len - 1]))
                len--;

            item = strndup(p, len);
            if (item != NULL) {
                /* strlist_push copies the string, so the temporary is
                 * released unconditionally afterwards -- on the failure path
                 * too, which is why it is not inside the if. */
                strlist_push(&e->files, &e->n_files, item);
                free(item);
            }
        }

        if (eol == NULL)
            break;
        p = eol + 1;
    }

    free(text);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

const char *zbra_db_default_root(void)
{
    const char *env = getenv("ZBRA_ROOT");

    if (env != NULL && *env != '\0')
        return env;

    return "/var/lib/zbra";
}

int zbra_db_open(zbra_db *db, const char *root)
{
    static const zbra_kind kinds[] = {
        ZBRA_KIND_PACKAGE, ZBRA_KIND_DEPENDENCY, ZBRA_KIND_ISOLATED
    };
    size_t i;

    if (db == NULL || root == NULL)
        return -1;

    db->root = strdup(root);
    if (db->root == NULL)
        return -1;

    if (zbra_mkdir_p(db->root, 0755) != 0) {
        free(db->root);
        db->root = NULL;
        return -1;
    }

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        char sub[PATH_MAX];

        snprintf(sub, sizeof(sub), "%s/%s", db->root, kind_dir_name(kinds[i]));
        if (zbra_mkdir_p(sub, 0755) != 0) {
            free(db->root);
            db->root = NULL;
            return -1;
        }
    }

    return 0;
}

void zbra_db_close(zbra_db *db)
{
    if (db == NULL)
        return;
    free(db->root);
    db->root = NULL;
}

int zbra_db_put(zbra_db *db, const zbra_entry *e)
{
    char *dir = NULL;
    char *mpath = NULL;
    char *fpath = NULL;
    char *text = NULL;
    size_t len = 0;
    size_t i;
    size_t flen = 0;
    size_t fcap;
    char *files = NULL;
    int rc = -1;

    if (db == NULL || e == NULL || e->name == NULL)
        return -1;

    /* Refuse to record an entry whose name could escape the database. */
    if (!zbra_name_is_safe(e->name)) {
        errno = EINVAL;
        return -1;
    }

    dir = db_path(db, e->kind, e->name, NULL);
    if (dir == NULL)
        return -1;

    if (zbra_mkdir_p(dir, 0755) != 0)
        goto out;

    if (entry_serialise(e, &text, &len) != 0)
        goto out;

    mpath = db_path(db, e->kind, e->name, "meta");
    fpath = db_path(db, e->kind, e->name, "files");
    if (mpath == NULL || fpath == NULL)
        goto out;

    if (write_file_atomic(mpath, text, len) != 0)
        goto out;

    /* Serialise the owned-file list. */
    fcap = 256;
    files = malloc(fcap);
    if (files == NULL)
        goto out;
    files[0] = '\0';

    for (i = 0; i < e->n_files; i++) {
        int need;

        /* Reject a path that could escape the install root at write time,
         * not only at read time: a hostile index must not be able to seed a
         * database that later deletes something important. */
        if (!zbra_path_is_safe(e->files[i]))
            continue;

        need = snprintf(files + flen, fcap - flen, "%s\n", e->files[i]);
        if (need < 0)
            goto out;

        if ((size_t)need >= fcap - flen) {
            size_t ncap = fcap * 2;
            char *nb;

            while ((size_t)need >= ncap - flen)
                ncap *= 2;

            nb = realloc(files, ncap);
            if (nb == NULL)
                goto out;
            files = nb;
            fcap = ncap;

            need = snprintf(files + flen, fcap - flen, "%s\n", e->files[i]);
            if (need < 0)
                goto out;
        }

        flen += (size_t)need;
    }

    if (write_file_atomic(fpath, files, flen) != 0)
        goto out;

    rc = 0;

out:
    free(dir);
    free(mpath);
    free(fpath);
    free(text);
    free(files);
    return rc;
}

static int db_get_kind(zbra_db *db, const char *name, zbra_kind kind,
                       zbra_entry *out)
{
    char *mpath;
    char *text;
    int rc;

    if (!zbra_name_is_safe(name))
        return 0;

    mpath = db_path(db, kind, name, "meta");
    if (mpath == NULL)
        return -1;

    text = read_file(mpath, NULL);
    free(mpath);

    if (text == NULL)
        return 0;

    rc = entry_parse(out, text, kind);
    free(text);

    if (rc != 0)
        return -1;

    /* The record's name is authoritative; fall back to the directory name
     * when the field is missing so a truncated file is still usable. */
    if (out->name == NULL || out->name[0] == '\0') {
        free(out->name);
        out->name = strdup(name);
    }

    entry_load_files(db, out, kind);
    return 1;
}

int zbra_db_get(zbra_db *db, const char *name, zbra_entry *out)
{
    /*
     * Search order matters: an explicit package shadows a dependency entry
     * of the same name, because that is the state the user asked for.
     */
    static const zbra_kind order[] = {
        ZBRA_KIND_PACKAGE, ZBRA_KIND_ISOLATED, ZBRA_KIND_DEPENDENCY
    };
    size_t i;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        int rc = db_get_kind(db, name, order[i], out);

        if (rc < 0)
            return -1;
        if (rc == 1)
            return 1;
    }

    return 0;
}

zbra_kind zbra_db_kind_of(zbra_db *db, const char *name)
{
    static const zbra_kind order[] = {
        ZBRA_KIND_PACKAGE, ZBRA_KIND_ISOLATED, ZBRA_KIND_DEPENDENCY
    };
    size_t i;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        char *mpath = db_path(db, order[i], name, "meta");
        int found;

        if (mpath == NULL)
            continue;

        found = (access(mpath, F_OK) == 0);
        free(mpath);

        if (found)
            return order[i];
    }

    return ZBRA_KIND_PACKAGE;
}

int zbra_db_forget(zbra_db *db, const char *name, zbra_kind kind)
{
    char *meta;
    char *files;
    int removed = 0;

    if (db == NULL || !zbra_name_is_safe(name))
        return -1;

    meta = db_path(db, kind, name, "meta");
    if (meta == NULL)
        return -1;

    if (access(meta, F_OK) == 0) {
        if (unlink(meta) != 0 && errno != ENOENT) {
            free(meta);
            return -1;
        }
        removed = 1;
    }
    free(meta);

    files = db_path(db, kind, name, "files");
    if (files != NULL) {
        unlink(files);
        free(files);
    }

    /*
     * Drop the entry's directory too. A leftover empty directory is not a
     * harmless artefact: zbra_db_list treats a directory as an installed
     * package, so without this a removed package keeps showing up as installed
     * and cannot be installed again.
     */
    {
        char *dir = db_path(db, kind, name, NULL);

        if (dir != NULL) {
            rmdir(dir);
            free(dir);
        }
    }

    return removed;
}

int zbra_db_add_dependent(zbra_db *db, const char *name,
                          const char *dependent)
{
    zbra_entry e;
    int rc;
    int found;

    if (db == NULL || name == NULL || dependent == NULL)
        return -1;

    found = zbra_db_get(db, name, &e);
    if (found < 0)
        return -1;
    if (found == 0)
        return 1;                   /* nothing installed under that name */

    rc = strlist_push(&e.dependents, &e.n_dependents, dependent);
    if (rc == 0)
        rc = zbra_db_put(db, &e);

    zbra_entry_free(&e);
    return rc;
}

int zbra_db_unrequire(zbra_db *db, const char *name, const char *dependent)
{
    zbra_entry e;
    size_t     i, k;
    int        found;

    if (db == NULL || name == NULL || dependent == NULL)
        return -1;

    found = zbra_db_get(db, name, &e);
    if (found < 0)
        return -1;
    if (found == 0)
        return 0;

    for (i = 0, k = 0; i < e.n_dependents; i++) {
        if (e.dependents[i] != NULL &&
            strcmp(e.dependents[i], dependent) == 0) {
            free(e.dependents[i]);
            e.dependents[i] = NULL;
            continue;
        }
        e.dependents[k++] = e.dependents[i];
        e.dependents[k] = NULL;
    }
    e.n_dependents = k;

    found = zbra_db_put(db, &e);
    zbra_entry_free(&e);

    return found == 0 ? 0 : -1;
}

int zbra_db_release_dependent(zbra_db *db, const char *name,
                              const char *dependent)
{
    zbra_entry e;
    size_t i, k;
    int found;
    int rc = 0;

    if (db == NULL || name == NULL || dependent == NULL)
        return -1;

    found = zbra_db_get(db, name, &e);
    if (found < 0)
        return -1;
    if (found == 0)
        return 0;

    /* Remove `dependent` from the back-reference list, preserving order. */
    for (i = 0, k = 0; i < e.n_dependents; i++) {
        if (e.dependents[i] != NULL &&
            strcmp(e.dependents[i], dependent) == 0) {
            free(e.dependents[i]);
            e.dependents[i] = NULL;
            continue;
        }
        e.dependents[k++] = e.dependents[i];
        e.dependents[k] = NULL;
    }
    e.n_dependents = k;

    /*
     * A dependency with no remaining dependents is garbage, but only if the
     * user never asked for it explicitly. An explicit package stays even
     * when nothing depends on it -- that is the difference between the two
     * registries.
     */
    if (e.n_dependents == 0 && e.kind == ZBRA_KIND_DEPENDENCY) {
        rc = zbra_db_forget(db, name, ZBRA_KIND_DEPENDENCY);
        if (rc > 0) {
            zbra_entry_free(&e);
            return 1;
        }
        rc = 0;
    } else {
        rc = zbra_db_put(db, &e);
    }

    zbra_entry_free(&e);
    return rc;
}

char **zbra_db_list(zbra_db *db, zbra_kind kind, size_t *n)
{
    static const zbra_kind all[] = {
        ZBRA_KIND_PACKAGE, ZBRA_KIND_DEPENDENCY, ZBRA_KIND_ISOLATED
    };
    char **list = calloc(1, sizeof(*list));
    size_t count = 0;
    size_t ki;

    if (list == NULL)
        return NULL;

    if (n != NULL)
        *n = 0;

    if (db == NULL)
        return list;

    for (ki = 0; ki < sizeof(all) / sizeof(all[0]); ki++) {
        char dirpath[PATH_MAX];
        DIR *d;
        struct dirent *de;
        if (kind != ZBRA_KIND_ANY && all[ki] != kind)
            continue;

        snprintf(dirpath, sizeof(dirpath), "%s/%s", db->root,
                 kind_dir_name(all[ki]));

        d = opendir(dirpath);
        if (d == NULL)
            continue;

        while ((de = readdir(d)) != NULL) {
            char child[PATH_MAX];
            struct stat st;

            if (de->d_name[0] == '.')
                continue;

            /*
             * Only directories are entries; stray files are ignored.
             * A name longer than the remaining room in `child` cannot be a
             * valid entry, so skipping it is both safe and what we want.
             */
            if (snprintf(child, sizeof(child), "%s/%s", dirpath, de->d_name) >=
                (int)sizeof(child))
                continue;

            if (stat(child, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;

            /*
             * A directory on its own is not an installed package; the entry
             * has to have a meta file in it. Requiring that means a directory
             * left behind by an interrupted removal, or by a crash between
             * unlinking meta and rmdir, is not reported as installed.
             */
            {
                char  meta[PATH_MAX];
                FILE *fp;

                if (snprintf(meta, sizeof(meta), "%s/meta", child) >=
                    (int)sizeof(meta))
                    continue;

                fp = fopen(meta, "r");
                if (fp == NULL)
                    continue;
                fclose(fp);
            }

            if (strlist_push(&list, &count, de->d_name) != 0) {
                closedir(d);
                zbra_strlist_free_owned(list);
                if (n != NULL)
                    *n = 0;
                return NULL;
            }
        }

        closedir(d);
    }

    if (n != NULL)
        *n = count;

    return list;
}

char *zbra_db_owner_of(zbra_db *db, const char *path)
{
    size_t n = 0;
    char **names;
    char *found = NULL;
    size_t i;

    if (db == NULL || path == NULL)
        return NULL;

    names = zbra_db_list(db, ZBRA_KIND_ANY, &n);
    if (names == NULL)
        return NULL;

    for (i = 0; i < n && found == NULL; i++) {
        zbra_entry e;

        if (zbra_db_get(db, names[i], &e) != 1)
            continue;

        {
            size_t f;
            for (f = 0; f < e.n_files; f++) {
                if (e.files[f] != NULL && strcmp(e.files[f], path) == 0) {
                    found = strdup(names[i]);
                    break;
                }
            }
        }

        zbra_entry_free(&e);
    }

    zbra_strlist_free_owned(names);
    return found;
}
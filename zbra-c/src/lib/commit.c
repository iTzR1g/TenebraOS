/*
 * Staged, reversible filesystem changes.
 *
 * The install path is the one place where a mistake is unrecoverable, so the
 * shape here is deliberately narrow:
 *
 *   - A backend unpacks into a private staging directory. Nothing it does can
 *     affect the live filesystem, so a hostile tarball with an absolute path
 *     or a ".." component writes junk into staging and nothing else.
 *   - Paths are validated when they are recorded, not when they are moved.
 *   - Apply moves files with rename(2) from a staging directory that lives on
 *     the same filesystem as the database, so a move is atomic and cheap.
 *   - Anything being overwritten is moved aside first and only deleted once
 *     every move has succeeded, so an interrupted upgrade can be rewound.
 *
 * What this deliberately does not do is pretend to be a filesystem
 * transaction. It cannot make a multi-file update atomic for an outside
 * observer, and it cannot undo a crash between moves without a journal on
 * disk. It does guarantee the property that matters for a package manager:
 * either the install completes, or the files it touched are as they were.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "commit.h"
#include "db.h"

typedef struct {
    char *rel;       /* payload path, relative to the staging directory */
    char *dest_rel;  /* where it lands, relative to the install root */
} commit_file;

struct zbra_commit {
    char        *parent;    /* where staging and backup live */
    char        *staging;
    char        *backup;
    commit_file *files;
    size_t       n;
    size_t       cap;

    size_t       applied;   /* moves that succeeded, for the undo path */
    size_t       backed;    /* originals moved into backup/ */
};

static char *mkerr(const char *fmt, ...)
{
    char    buf[1024];
    va_list ap;
    char   *out;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    out = strdup(buf);
    return out;
}

/* "a" + "/" + "b", tolerating a trailing slash on the left. */
static char *join(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char  *out;

    while (la > 1 && a[la - 1] == '/')
        la--;

    out = malloc(la + lb + 2);
    if (out == NULL)
        return NULL;

    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';

    return out;
}

/* ---------------------------------------------------------- tree removal */

/*
 * Recursively remove a tree.
 *
 * Only ever called on staging and backup directories this module created with
 * mkdtemp under a caller-chosen parent, so the path is trusted; lstat is used
 * rather than stat so a symlink is unlinked instead of followed, which is what
 * keeps a hostile payload from steering the walk out of the tree.
 */
static int rm_rf(const char *path)
{
    struct stat st;
    DIR        *dir;
    struct dirent *de;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;

    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    dir = opendir(path);
    if (dir == NULL)
        return -1;

    while ((de = readdir(dir)) != NULL) {
        char *child;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        child = join(path, de->d_name);
        if (child == NULL) {
            closedir(dir);
            return -1;
        }

        if (rm_rf(child) != 0) {
            int saved = errno;
            free(child);
            closedir(dir);
            errno = saved;
            return -1;
        }

        free(child);
    }

    closedir(dir);

    return rmdir(path);
}

/* ------------------------------------------------------------- lifecycle */

int zbra_commit_open(zbra_commit **out, const char *staging_parent,
                     char **err)
{
    zbra_commit *c;
    char         tmpl[PATH_MAX];
    const char  *slash;
    int          n;

    if (out == NULL || staging_parent == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_commit_open: bad arguments");
        return -1;
    }

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        if (err != NULL)
            *err = mkerr("out of memory");
        return -1;
    }

    c->parent = strdup(staging_parent);
    if (c->parent == NULL) {
        free(c);
        if (err != NULL)
            *err = mkerr("out of memory");
        return -1;
    }

    if (zbra_mkdir_p(c->parent, 0755) != 0) {
        if (err != NULL)
            *err = mkerr("cannot create %s: %s", c->parent, strerror(errno));
        free(c->parent);
        free(c);
        return -1;
    }

    /*
     * Staging is a sibling of the database rather than /tmp, so the moves in
     * apply stay within one filesystem and remain renames. Crossing devices
     * would turn every file into a copy and make the undo path unreliable.
     */
    slash = strrchr(c->parent, '/');
    if (slash != NULL && slash != c->parent)
        n = snprintf(tmpl, sizeof(tmpl), "%.*s/zbra-stage-XXXXXX",
                     (int)(slash - c->parent), c->parent);
    else if (slash == c->parent)
        n = snprintf(tmpl, sizeof(tmpl), "/zbra-stage-XXXXXX");
    else
        n = snprintf(tmpl, sizeof(tmpl), "zbra-stage-XXXXXX");

    if (n < 0 || (size_t)n >= sizeof(tmpl)) {
        if (err != NULL)
            *err = mkerr("staging path is too long");
        free(c->parent);
        free(c);
        return -1;
    }

    if (mkdtemp(tmpl) == NULL) {
        if (err != NULL)
            *err = mkerr("cannot create a staging directory: %s",
                         strerror(errno));
        free(c->parent);
        free(c);
        return -1;
    }

    c->staging = strdup(tmpl);
    if (c->staging == NULL) {
        rm_rf(tmpl);
        free(c->parent);
        free(c);
        if (err != NULL)
            *err = mkerr("out of memory");
        return -1;
    }

    *out = c;
    return 0;
}

const char *zbra_commit_staging(const zbra_commit *c)
{
    return c != NULL ? c->staging : NULL;
}

const char *zbra_commit_dest(const zbra_commit *c, size_t i)
{
    if (c == NULL || i >= c->n)
        return NULL;

    return c->files[i].dest_rel;
}

size_t zbra_commit_count(const zbra_commit *c)
{
    return c != NULL ? c->n : 0;
}

int zbra_commit_add(zbra_commit *c, const char *rel, const char *dest_rel,
                    char **err)
{
    if (c == NULL || rel == NULL || dest_rel == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_commit_add: bad arguments");
        return -1;
    }

    if (c->n == c->cap) {
        size_t       cap = c->cap != 0 ? c->cap * 2 : 32;
        commit_file *grown = realloc(c->files, cap * sizeof(*grown));

        if (grown == NULL) {
            if (err != NULL)
                *err = mkerr("out of memory");
            return -1;
        }

        c->files = grown;
        c->cap  = cap;
    }

    /*
     * Validate both paths up front. By the time apply runs it is moving real
     * files with no time left to reconsider, and the payload is untrusted
     * input: a tarball that lists "../../etc/shadow" must be refused here.
     */
    if (!zbra_path_is_safe(rel) || !zbra_path_is_safe(dest_rel)) {
        if (err != NULL)
            *err = mkerr("refusing unsafe path \"%s\" -> \"%s\"", rel,
                         dest_rel);
        return -1;
    }

    c->files[c->n].rel      = strdup(rel);
    c->files[c->n].dest_rel = strdup(dest_rel);

    if (c->files[c->n].rel == NULL || c->files[c->n].dest_rel == NULL) {
        free(c->files[c->n].rel);
        free(c->files[c->n].dest_rel);
        if (err != NULL)
            *err = mkerr("out of memory");
        return -1;
    }

    c->n++;
    return 0;
}

void zbra_commit_abort(zbra_commit *c)
{
    size_t i;

    if (c == NULL)
        return;

    for (i = 0; i < c->n; i++) {
        free(c->files[i].rel);
        free(c->files[i].dest_rel);
    }
    free(c->files);

    if (c->backup != NULL) {
        rm_rf(c->backup);
        free(c->backup);
    }

    rm_rf(c->staging);
    free(c->staging);
    free(c->parent);
    free(c);
}

/* ----------------------------------------------------------------- apply */

/*
 * Undo the moves already made.
 *
 * Walks the applied prefix in reverse so directories empty out in step with
 * the order they were filled. Files that replaced something are restored from
 * backup rather than simply removed.
 *
 * `install_root` matters here: destinations are relative to it, not to the
 * staging parent, so the undo path has to look in the same place the forward
 * path wrote.
 */
static void undo_moves(zbra_commit *c, const char *install_root)
{
    while (c->applied > 0) {
        size_t i = --c->applied;
        char  *from;
        char  *to;

        from = join(c->staging, c->files[i].rel);
        to   = join(install_root, c->files[i].dest_rel);

        if (from != NULL && to != NULL)
            (void)rename(to, from);

        /*
         * If this index also displaced an original, put the original back
         * now that our copy has been lifted out of the way.
         */
        if (i < c->backed) {
            char  *orig;
            char   name[32];

            snprintf(name, sizeof(name), "%zu", i);
            orig = join(c->backup, name);

            if (orig != NULL) {
                (void)rename(orig, to);
                free(orig);
            }
        }

        free(from);
        free(to);
    }
}

int zbra_commit_apply(zbra_commit **cp, const char *install_root, char **err)
{
    zbra_commit *c;
    size_t       i;
    int          rc = -1;

    if (cp == NULL || *cp == NULL || install_root == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_commit_apply: bad arguments");
        return -1;
    }

    c = *cp;

    if (c->n == 0) {
        if (err != NULL)
            *err = mkerr("nothing to install");
        return -1;
    }

    /*
     * Preflight: every conflict is found before the first move, so the common
     * failure (a file the system or another package already owns) costs
     * nothing and leaves nothing behind.
     */
    for (i = 0; i < c->n; i++) {
        char         *dest = join(install_root, c->files[i].dest_rel);
        struct stat   st;

        if (dest == NULL) {
            if (err != NULL)
                *err = mkerr("out of memory");
            return -1;
        }

        if (lstat(dest, &st) == 0 && !S_ISREG(st.st_mode)) {
            if (err != NULL)
                *err = mkerr("%s already exists and is not a regular file",
                             dest);
            free(dest);
            return -1;
        }

        free(dest);
    }

    if (c->backup == NULL) {
        char tmpl[PATH_MAX];

        snprintf(tmpl, sizeof(tmpl), "%s/zbra-backup-XXXXXX", c->parent);
        if (mkdtemp(tmpl) == NULL) {
            if (err != NULL)
                *err = mkerr("cannot create a backup directory: %s",
                             strerror(errno));
            return -1;
        }

        c->backup = strdup(tmpl);
        if (c->backup == NULL) {
            rm_rf(tmpl);
            if (err != NULL)
                *err = mkerr("out of memory");
            return -1;
        }
    }

    for (i = 0; i < c->n; i++) {
        char *src = join(c->staging, c->files[i].rel);
        char *dst = join(install_root, c->files[i].dest_rel);
        char *parent;
        char *slash;

        if (src == NULL || dst == NULL) {
            free(src);
            free(dst);
            if (err != NULL)
                *err = mkerr("out of memory");
            goto out;
        }

        /* Directories are created on demand, so a package need not ship them. */
        parent = strdup(dst);
        if (parent == NULL) {
            free(src);
            free(dst);
            if (err != NULL)
                *err = mkerr("out of memory");
            goto out;
        }

        slash = strrchr(parent, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (zbra_mkdir_p(parent, 0755) != 0) {
                if (err != NULL)
                    *err = mkerr("cannot create %s: %s", parent,
                                 strerror(errno));
                free(parent);
                free(src);
                free(dst);
                goto out;
            }
        }
        free(parent);

        /*
         * Move any displaced original aside before overwriting, so the undo
         * path can restore it. Backups are numbered by index, which is what
         * undo_moves replays in reverse.
         */
        {
            struct stat st;
            char        name[32];
            char       *slot;

            if (lstat(dst, &st) == 0) {
                snprintf(name, sizeof(name), "%zu", i);
                slot = join(c->backup, name);
                if (slot == NULL) {
                    free(src);
                    free(dst);
                    if (err != NULL)
                        *err = mkerr("out of memory");
                    goto out;
                }

                if (rename(dst, slot) != 0) {
                    if (err != NULL)
                        *err = mkerr("cannot set %s aside: %s", dst,
                                     strerror(errno));
                    free(slot);
                    free(src);
                    free(dst);
                    goto out;
                }

                free(slot);
                c->backed = i + 1;
            }
        }

        if (rename(src, dst) != 0) {
            if (err != NULL)
                *err = mkerr("cannot install %s: %s", c->files[i].dest_rel,
                             strerror(errno));
            free(src);
            free(dst);
            goto out;
        }

        free(src);
        free(dst);
        c->applied++;
    }

    rc = 0;

out:
    if (rc != 0)
        undo_moves(c, install_root);

    if (rc == 0) {
        /* The originals are only worthless once every move has landed. */
        if (c->backup != NULL)
            rm_rf(c->backup);
        zbra_commit_abort(c);
        *cp = NULL;
    }

    return rc;
}
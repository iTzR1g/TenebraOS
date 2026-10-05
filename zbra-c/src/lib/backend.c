/*
 * Backend registry, plus the adoption walk shared by every backend.
 *
 * Each backend lives in its own file with a two-function interface, declared
 * here so the table can stay a plain array rather than a registry with
 * registration order to reason about.
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "backend_impl.h"
#include "db.h"

static const zbra_backend_impl k_backends[] = {
    { "tar.xz", "tar.xz", zbra_backend_tar_claims, zbra_backend_tar_stage,
      zbra_backend_tar_available },
    { "deb", "deb", zbra_backend_deb_claims, zbra_backend_deb_stage,
      zbra_backend_deb_available },
    { "rpm", "rpm", zbra_backend_rpm_claims, zbra_backend_rpm_stage,
      zbra_backend_rpm_available },
    { "aur", "aur", zbra_backend_aur_claims, zbra_backend_aur_stage,
      zbra_backend_aur_available },
    { "snap", "snap", zbra_backend_snap_claims, zbra_backend_snap_stage,
      zbra_backend_snap_available },
};

#define NBACKENDS (sizeof(k_backends) / sizeof(k_backends[0]))

static const zbra_backend_impl *find(const char *b)
{
    size_t i;

    if (b == NULL)
        return NULL;

    for (i = 0; i < NBACKENDS; i++) {
        if (strcmp(k_backends[i].name, b) == 0)
            return &k_backends[i];
    }

    return NULL;
}

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

const char *const *zbra_backend_names(void)
{
    static const char *names[NBACKENDS + 1];
    static int         ready;
    size_t             i;

    if (!ready) {
        for (i = 0; i < NBACKENDS; i++)
            names[i] = k_backends[i].name;
        names[NBACKENDS] = NULL;
        ready = 1;
    }

    return names;
}

int zbra_backend_available(const char *b, char **why)
{
    const zbra_backend_impl *impl = find(b);

    if (impl == NULL) {
        if (why != NULL)
            *why = mkerr("no backend named \"%s\"", b != NULL ? b : "(null)");
        return 0;
    }

    if (impl->available == NULL)
        return 1;

    return impl->available(why);
}

int zbra_backend_claims(const char *b, const char *format,
                        const char *filename)
{
    const zbra_backend_impl *impl = find(b);

    if (impl == NULL)
        return 0;

    if (impl->claims == NULL)
        return 0;

    return impl->claims(format, filename);
}

int zbra_backend_stage(const char *b, zbra_commit *c, const char *path,
                       const zbra_package *pkg, char **err)
{
    const zbra_backend_impl *impl = find(b);

    if (impl == NULL) {
        if (err != NULL)
            *err = mkerr("no backend named \"%s\"", b != NULL ? b : "(null)");
        return -1;
    }

    return impl->stage(c, path, pkg, err);
}

/* ---------------------------------------------------------------- adopt */

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

/*
 * Decide whether a packaged symlink stays inside the install root.
 *
 * Symlinks are legitimate and common: a library soname pointing at
 * "../lib/libfoo.so.1", an alternatives entry pointing into /usr/share, a
 * busybox applet pointing back at busybox. Banning ".." outright would refuse
 * a large share of real packages, so instead the target is resolved
 * lexically against the link's own directory and the question asked is simply
 * whether the result is still inside the prefix.
 *
 * The resolution is textual rather than via realpath() on purpose: the file
 * may not exist yet, and the destination is relative to an install root that
 * is usually "/", where every prefix lookup would succeed and tell us nothing.
 */
static int symlink_is_safe(const char *dst_dir, const char *target)
{
    char   work[8192];
    char  *parts[512];
    size_t nparts = 0;
    char  *p;
    int    safe = 1;

    if (target == NULL || *target == '\0')
        return 0;

    /* An absolute target is a pointer out of the prefix by definition. */
    if (target[0] == '/')
        return 0;

    if (snprintf(work, sizeof(work), "%s/%s", dst_dir != NULL ? dst_dir : "",
                 target) >= (int)sizeof(work))
        return 0;

    p = work;
    while (*p != '\0') {
        char *slash = strchr(p, '/');
        size_t len = slash != NULL ? (size_t)(slash - p) : strlen(p);

        if (len == 0 || (len == 1 && p[0] == '.')) {
            /* "" or ".": contributes nothing. */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (nparts == 0) {
                /* Climbing above the root: this is the escape case. */
                safe = 0;
                break;
            }
            nparts--;
        } else {
            if (nparts >= sizeof(parts) / sizeof(parts[0])) {
                safe = 0;
                break;
            }
            parts[nparts++] = p;
            (void)len;
        }

        if (slash == NULL)
            break;
        p = slash + 1;
    }

    /*
     * A link that resolves to the root itself names a directory, not a file,
     * so treat it as hostile too rather than working out the intent.
     */
    if (safe && nparts == 0)
        safe = 0;

    return safe;
}

/*
 * The adoption walk carries two paths for every entry.
 *
 * `src` is where the file is now, relative to the staging directory, and
 * includes the scratch prefix the backend unpacked into. `dst` is where the
 * package means for it to live, relative to the install root. Conflating the
 * two is how "_unpack/usr/bin/tool" ends up installed as a real directory
 * called _unpack.
 */
static int adopt_dir(zbra_commit *c, const char *root, const char *src,
                     const char *dst, char **err);

static int adopt_entry(zbra_commit *c, const char *root, const char *src,
                       const char *dst, char **err)
{
    char        full[8192];
    struct stat st;

    if (snprintf(full, sizeof(full), "%s/%s", root, src) >= (int)sizeof(full)) {
        if (err != NULL)
            *err = mkerr("path is too long: %s", src);
        return -1;
    }

    if (lstat(full, &st) != 0) {
        if (err != NULL)
            *err = mkerr("cannot read %s: %s", src, strerror(errno));
        return -1;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(full, target, sizeof(target) - 1);

        if (n < 0) {
            if (err != NULL)
                *err = mkerr("cannot read the link %s: %s", src,
                             strerror(errno));
            return -1;
        }
        target[n] = '\0';

        {
            char parent[4096];
            char *slash;

            /* The link's destination directory, which the target hangs off. */
            snprintf(parent, sizeof(parent), "%s", dst);
            slash = strrchr(parent, '/');
            if (slash != NULL)
                *slash = '\0';
            else
                parent[0] = '\0';

            if (!symlink_is_safe(parent, target)) {
                if (err != NULL)
                    *err = mkerr("refusing link %s -> %s: it points outside "
                                 "the install root",
                                 dst, target);
                return -1;
            }
        }

        return zbra_commit_add(c, src, dst, err);
    }

    if (S_ISREG(st.st_mode))
        return zbra_commit_add(c, src, dst, err);

    if (S_ISDIR(st.st_mode))
        return adopt_dir(c, root, src, dst, err);

    /*
     * Device nodes, fifos and sockets have no business in a payload: they are
     * how a package would escape a container or a chroot. Refusing the whole
     * install is the safe answer.
     */
    if (err != NULL)
        *err = mkerr("refusing %s: unsupported file type", dst);

    return -1;
}

/* Join only when there is something to join, so "." never leaks into a path. */
static char *child_path(const char *base, const char *name)
{
    if (base == NULL || *base == '\0' || strcmp(base, ".") == 0)
        return strdup(name);

    return join(base, name);
}

static int adopt_dir(zbra_commit *c, const char *root, const char *src,
                     const char *dst, char **err)
{
    char full[8192];
    DIR *dir;
    struct dirent *de;

    if (snprintf(full, sizeof(full), "%s/%s", root, src) >= (int)sizeof(full)) {
        if (err != NULL)
            *err = mkerr("path is too long: %s", src);
        return -1;
    }

    dir = opendir(full);
    if (dir == NULL) {
        if (err != NULL)
            *err = mkerr("cannot read %s: %s", src, strerror(errno));
        return -1;
    }

    while ((de = readdir(dir)) != NULL) {
        char *child_src;
        char *child_dst;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        child_src = child_path(src, de->d_name);
        child_dst = child_path(dst, de->d_name);

        if (child_src == NULL || child_dst == NULL) {
            free(child_src);
            free(child_dst);
            closedir(dir);
            if (err != NULL)
                *err = mkerr("out of memory");
            return -1;
        }

        if (adopt_entry(c, root, child_src, child_dst, err) != 0) {
            free(child_src);
            free(child_dst);
            closedir(dir);
            return -1;
        }

        free(child_src);
        free(child_dst);
    }

    closedir(dir);

    return 0;
}

int zbra_backend_adopt(zbra_commit *c, const char *prefix, char **err)
{
    const char *staging;

    if (c == NULL || prefix == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_adopt: bad arguments");
        return -1;
    }

    staging = zbra_commit_staging(c);

    return adopt_dir(c, staging, prefix, "", err);
}
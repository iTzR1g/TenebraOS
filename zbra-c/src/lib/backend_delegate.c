/*
 * Delegated backends: rpm, aur and snap.
 *
 * Each of these formats belongs to another package manager with years of
 * accumulated edge cases -- rpm signatures and scriptlets, PKGBUILD hooks,
 * snap confinement -- so zbra does not reimplement them. It runs the owning
 * tool and adopts whatever it produced.
 *
 * What delegation does not hand over is trust: the result still goes through
 * zbra_backend_adopt, so an rpm cannot install a device node or a link out of
 * the prefix that a tar.xz would have been refused for.
 *
 * Each backend says plainly when its tool is missing. "install failed" is a
 * useless message when the real answer is "this image has no rpm support".
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backend.h"
#include "backend_impl.h"
#include "db.h"
#include "proc.h"

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

/*
 * The file name for messages. NULL-safe because it is called from
 * initialisers that run before argument validation.
 */
static const char *base_name(const char *path)
{
    const char *slash;

    if (path == NULL)
        return "(none)";

    slash = strrchr(path, '/');

    return slash != NULL ? slash + 1 : path;
}

/* Whether a tool exited cleanly, complaining in detail if it did not. */
static int tool_ok(int status, const char *tool, const char *what,
                   const char *tool_err, char **err)
{
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        if (err != NULL)
            *err = mkerr("%s is not installed, so this package cannot be "
                         "installed", tool);
        return 0;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 126) {
        if (err != NULL)
            *err = mkerr("%s could not %s: it could not be started in the "
                         "build directory", tool, what);
        return 0;
    }

    if (err != NULL)
        *err = mkerr("%s could not %s%s%s", tool, what,
                     tool_err != NULL ? ": " : "",
                     tool_err != NULL ? tool_err : "");

    return 0;
}

/* ---------------------------------------------------------------- rpm */

int zbra_backend_rpm_claims(const char *format, const char *filename)
{
    if (format != NULL && strcmp(format, "rpm") == 0)
        return 1;

    if (filename == NULL)
        return 0;

    return strstr(filename, ".rpm") != NULL;
}

int zbra_backend_rpm_available(char **why)
{
    if (zbra_have_program("rpm2cpio") && zbra_have_program("cpio"))
        return 1;

    if (why != NULL)
        *why = mkerr("installing .rpm packages needs rpm2cpio(1) and cpio(1), "
                     "which are not installed");

    return 0;
}

int zbra_backend_rpm_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err)
{
    char        staging[4096];
    char        cpio_file[8192];
    const char *argv[16];
    char       *tool_err = NULL;
    int         status = 0;
    size_t      n = 0;

    (void)pkg;

    if (c == NULL || path == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_rpm_stage: bad arguments");
        return -1;
    }

    if (!zbra_backend_rpm_available(NULL)) {
        if (err != NULL)
            *err = mkerr("cannot install %s: this image has no rpm tools",
                         base_name(path));
        return -1;
    }

    snprintf(staging, sizeof(staging), "%s/_unpack", zbra_commit_staging(c));
    snprintf(cpio_file, sizeof(cpio_file), "%s/payload.cpio", staging);

    if (zbra_mkdir_p(staging, 0755) != 0) {
        if (err != NULL)
            *err = mkerr("cannot create %s: %s", staging, strerror(errno));
        return -1;
    }

    /*
     * rpm2cpio is a filter: rpm in, cpio out. The cpio stream is written to a
     * file first rather than piped straight into cpio, so a failure on either
     * side leaves nothing half-extracted.
     */
    n = 0;
    argv[n++] = "rpm2cpio";
    argv[n++] = path;
    argv[n] = NULL;

    if (zbra_run_to_file(argv, cpio_file, &tool_err, &status) != 0) {
        if (err != NULL)
            *err = tool_err != NULL ? tool_err : mkerr("cannot run rpm2cpio");
        else
            free(tool_err);
        return -1;
    }

    if (!tool_ok(status, "rpm2cpio", "read the package", NULL, err))
        return -1;

    n = 0;
    argv[n++] = "cpio";
    argv[n++] = "--extract";
    argv[n++] = "--make-directories";
    argv[n++] = "--no-absolute-filenames";
    argv[n++] = "--quiet";
    argv[n] = NULL;

    if (zbra_run_at(staging, cpio_file, argv, NULL, &tool_err, &status) != 0) {
        if (err != NULL)
            *err = tool_err != NULL ? tool_err : mkerr("cannot run cpio");
        else
            free(tool_err);
        return -1;
    }

    if (!tool_ok(status, "cpio", "unpack the package", tool_err, err)) {
        free(tool_err);
        return -1;
    }

    free(tool_err);

    return zbra_backend_adopt(c, "_unpack", err);
}

/* ---------------------------------------------------------------- aur */

int zbra_backend_aur_claims(const char *format, const char *filename)
{
    if (format != NULL && strcmp(format, "aur") == 0)
        return 1;

    /* An AUR source is a PKGBUILD, not an archive of its own. */
    if (filename != NULL && strstr(filename, "PKGBUILD") != NULL)
        return 1;

    return 0;
}

int zbra_backend_aur_available(char **why)
{
    if (zbra_have_program("makepkg") || zbra_have_program("pkgctl"))
        return 1;

    if (why != NULL)
        *why = mkerr("installing an AUR package needs makepkg(1) or pkgctl, "
                     "which are not installed");

    return 0;
}

/* Copy a file, creating parent directories. Used to stage PKGBUILD trees. */
static int copy_file_to(const char *src, const char *dst)
{
    char   buf[8192];
    FILE  *in;
    FILE  *out;
    size_t got;

    in = fopen(src, "rb");
    if (in == NULL)
        return -1;

    out = fopen(dst, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }

    while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, got, out) != got) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }

    fclose(in);
    return fclose(out) == 0 ? 0 : -1;
}

/* Recursively copy a PKGBUILD directory next to the one zbra will build. */
static int copy_tree(const char *src, const char *dst)
{
    DIR           *dir;
    struct dirent *de;

    if (zbra_mkdir_p(dst, 0755) != 0)
        return -1;

    dir = opendir(src);
    if (dir == NULL)
        return -1;

    while ((de = readdir(dir)) != NULL) {
        char        from[8192];
        char        to[8192];
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        snprintf(from, sizeof(from), "%s/%s", src, de->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, de->d_name);

        if (lstat(from, &st) != 0)
            continue;

        /* Symlinks in a PKGBUILD are relative by convention; copy the link. */
        if (S_ISLNK(st.st_mode)) {
            char  target[4096];
            ssize_t n = readlink(from, target, sizeof(target) - 1);

            if (n < 0)
                continue;
            target[n] = '\0';
            (void)unlink(to);
            if (symlink(target, to) != 0)
                continue;
        } else if (S_ISDIR(st.st_mode)) {
            if (copy_tree(from, to) != 0) {
                closedir(dir);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (copy_file_to(from, to) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return 0;
}

int zbra_backend_aur_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err)
{
    char        build[4096];
    const char *argv[12];
    char       *tool_err = NULL;
    int         status = 0;
    size_t      n = 0;
    const char *tool;
    struct stat st;

    (void)pkg;

    if (c == NULL || path == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_aur_stage: bad arguments");
        return -1;
    }

    if (!zbra_backend_aur_available(NULL)) {
        if (err != NULL)
            *err = mkerr("cannot install %s: this image has no AUR tools",
                         base_name(path));
        return -1;
    }

    tool = zbra_have_program("makepkg") ? "makepkg" : "pkgctl";

    /*
     * makepkg builds in its working directory and caches under $HOME, so it
     * gets a private scratch copy rather than writing into the user's home or
     * picking up whatever is already there.
     */
    snprintf(build, sizeof(build), "%s/_aur", zbra_commit_staging(c));

    if (zbra_mkdir_p(build, 0755) != 0) {
        if (err != NULL)
            *err = mkerr("cannot create %s: %s", build, strerror(errno));
        return -1;
    }

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (copy_tree(path, build) != 0) {
            if (err != NULL)
                *err = mkerr("cannot stage the AUR sources from %s", path);
            return -1;
        }
    } else {
        char dest[8192];

        snprintf(dest, sizeof(dest), "%s/PKGBUILD", build);
        if (copy_file_to(path, dest) != 0) {
            if (err != NULL)
                *err = mkerr("cannot read %s: %s", path, strerror(errno));
            return -1;
        }
    }

    if (strcmp(tool, "makepkg") == 0) {
        /*
         * Only the packaging steps run here: makepkg --nobuild executes the
         * PKGBUILD's pkg() to lay out files, which is exactly the tree zbra
         * wants to stage. Compiling is left to the normal build path.
         */
        n = 0;
        argv[n++] = "makepkg";
        argv[n++] = "--nobuild";
        argv[n++] = "--nodeps";
        argv[n++] = "--noconfirm";
        argv[n] = NULL;
    } else {
        n = 0;
        argv[n++] = "pkgctl";
        argv[n++] = "build";
        argv[n++] = "--no-deps";
        argv[n] = NULL;
    }

    if (zbra_run_at(build, NULL, argv, NULL, &tool_err, &status) != 0) {
        if (err != NULL)
            *err = tool_err != NULL ? tool_err : mkerr("cannot run %s", tool);
        else
            free(tool_err);
        return -1;
    }

    if (!tool_ok(status, tool, "build the package", tool_err, err)) {
        free(tool_err);
        return -1;
    }

    free(tool_err);

    /*
     * src/ is where makepkg lays out the package tree; adopt only that, since
     * the build directory also holds the PKGBUILD itself and its cache.
     */
    {
        char src_dir[8192];
        struct stat st2;

        snprintf(src_dir, sizeof(src_dir), "%s/src", build);
        if (lstat(src_dir, &st2) == 0 && S_ISDIR(st2.st_mode))
            return zbra_backend_adopt(c, "_aur/src", err);
    }

    if (err != NULL)
        *err = mkerr("%s built no package tree; the PKGBUILD may be missing "
                     "a pkg() function", tool);

    return -1;
}

/* --------------------------------------------------------------- snap */

int zbra_backend_snap_claims(const char *format, const char *filename)
{
    if (format != NULL && strcmp(format, "snap") == 0)
        return 1;

    if (filename == NULL)
        return 0;

    return strstr(filename, ".snap") != NULL;
}

int zbra_backend_snap_available(char **why)
{
    if (zbra_have_program("snap"))
        return 1;

    if (why != NULL)
        *why = mkerr("installing a snap needs snapd(8), which is not installed");

    return 0;
}

int zbra_backend_snap_stage(zbra_commit *c, const char *path,
                            const zbra_package *pkg, char **err)
{
    const char *argv[8];
    char       *tool_err = NULL;
    int         status = 0;
    size_t      n = 0;

    (void)path;

    if (c == NULL || pkg == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_snap_stage: bad arguments");
        return -1;
    }

    if (!zbra_backend_snap_available(NULL)) {
        if (err != NULL)
            *err = mkerr("cannot install %s: snapd is not installed",
                         pkg->name != NULL ? pkg->name : "this snap");
        return -1;
    }

    if (pkg->name == NULL || *pkg->name == '\0') {
        if (err != NULL)
            *err = mkerr("cannot install a snap without a name");
        return -1;
    }

    /*
     * A snap is not a filesystem payload. snapd owns a squashfs mount that the
     * system references by revision, and it manages the mount namespace,
     * confinement and interface wiring itself. Copying its files into the
     * prefix would produce something that looks installed and behaves like a
     * random directory, so the whole operation is handed over and nothing is
     * staged. The caller must not treat this as an install with zero files.
     */
    n = 0;
    argv[n++] = "snap";
    argv[n++] = "install";
    argv[n++] = pkg->name;
    argv[n] = NULL;

    if (zbra_run_capture(argv, NULL, &tool_err, &status) != 0) {
        if (err != NULL)
            *err = tool_err != NULL ? tool_err : mkerr("cannot run snap");
        else
            free(tool_err);
        return -1;
    }

    if (!tool_ok(status, "snap", "install the package", tool_err, err)) {
        free(tool_err);
        return -1;
    }

    free(tool_err);

    return 0;
}
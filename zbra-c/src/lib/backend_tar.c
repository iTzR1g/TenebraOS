/*
 * tar.xz: the format TenebraOS publishes, unpacked natively.
 *
 * tar(1) and xz(1) do the decompression. That is not a compromise about
 * independence -- they are core system tools, present on every image zbra
 * would run on -- whereas delegating to dpkg or rpm would mean those package
 * managers had to be installed first, which is precisely what a TenebraOS
 * package is supposed to avoid.
 *
 * Everything that decides *which* files get installed stays here, so the
 * external tool is only ever asked to decompress bytes.
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

int zbra_backend_tar_claims(const char *format, const char *filename)
{
    if (format != NULL && strcmp(format, "tar.xz") == 0)
        return 1;

    if (filename == NULL)
        return 0;

    return strstr(filename, ".tar.xz") != NULL;
}

int zbra_backend_tar_available(char **why)
{
    if (!zbra_have_program("tar")) {
        if (why != NULL)
            *why = mkerr("tar.xz packages need tar(1), which is not installed");
        return 0;
    }

    if (!zbra_have_program("xz")) {
        if (why != NULL)
            *why = mkerr("tar.xz packages need xz(1), which is not installed");
        return 0;
    }

    return 1;
}

int zbra_backend_tar_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err)
{
    char           staging[4096];
    const char    *staging_dir;
    const char    *argv[12];
    char          *tool_err = NULL;
    int            status = 0;
    size_t         n = 0;

    if (c == NULL || path == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_tar_stage: bad arguments");
        return -1;
    }

    if (!zbra_have_program("tar") || !zbra_have_program("xz")) {
        if (err != NULL)
            *err = mkerr("cannot install a tar.xz package: tar(1) and xz(1) "
                         "are both required");
        return -1;
    }

    if (access(path, R_OK) != 0) {
        if (err != NULL)
            *err = mkerr("cannot read %s: %s", path, strerror(errno));
        return -1;
    }

    staging_dir = zbra_commit_staging(c);

    /*
     * Tar archives are allowed one leading directory, so "package-1.0/"
     * wrapping the real tree is normal. Unpack into a scratch directory and
     * strip that component when adopting, rather than guessing up front.
     */
    snprintf(staging, sizeof(staging), "%s/_unpack", staging_dir);
    if (zbra_mkdir_p(staging, 0755) != 0) {
        if (err != NULL)
            *err = mkerr("cannot create %s: %s", staging, strerror(errno));
        return -1;
    }

    argv[n++] = "tar";
    argv[n++] = "--extract";
    argv[n++] = "--xz";
    argv[n++] = "--no-same-owner";
    argv[n++] = "--no-same-permissions";
    argv[n++] = "--directory";
    argv[n++] = staging;
    argv[n++] = "--file";
    argv[n++] = path;
    argv[n] = NULL;

    if (zbra_run_capture(argv, NULL, &tool_err, &status) != 0) {
        if (err != NULL)
            *err = tool_err != NULL ? tool_err
                                    : mkerr("cannot run tar");
        else
            free(tool_err);
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (err != NULL)
            *err = mkerr("cannot unpack %s: tar exited badly%s%s", base_name(path),
                         tool_err != NULL ? ": " : "",
                         tool_err != NULL ? tool_err : "");
        else
            free(tool_err);
        free(tool_err);
        return -1;
    }

    free(tool_err);
    tool_err = NULL;

    /*
     * Adopt the tree. A tarball conventionally wraps its payload in one
     * top-level directory, and that wrapper is not part of the install layout,
     * so strip it -- but only when there is exactly one and it is not "usr",
     * which is the case where guessing would be wrong.
     */
    {
        DIR           *dir = opendir(staging);
        struct dirent *de;
        int            entries = 0;
        char           only[1024] = "";
        char           prefix[4096];

        if (dir == NULL) {
            if (err != NULL)
                *err = mkerr("cannot read %s: %s", staging, strerror(errno));
            return -1;
        }

        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            snprintf(only, sizeof(only), "%s", de->d_name);
            entries++;
        }
        closedir(dir);

        if (entries == 0) {
            if (err != NULL)
                *err = mkerr("%s unpacked to nothing", base_name(path));
            return -1;
        }

        if (entries == 1 && strcmp(only, "usr") != 0) {
            struct stat st;
            char        candidate[8192];

            snprintf(candidate, sizeof(candidate), "%s/%s", staging, only);
            if (lstat(candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(prefix, sizeof(prefix), "_unpack/%s", only);
                return zbra_backend_adopt(c, prefix, err);
            }
        }

        snprintf(prefix, sizeof(prefix), "_unpack");
    }

    {
        int rc = zbra_backend_adopt(c, "_unpack", err);

        return rc;
    }
}
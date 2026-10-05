/*
 * Debian packages.
 *
 * A .deb is an ar archive holding control.tar.* and data.tar.*, so the useful
 * question is only "which tool can unpack this". dpkg-deb is preferred because
 * it validates the control member and unpacks the right data member whatever
 * the compression; ar(1) plus tar is the fallback for a system that has the
 * tools but not dpkg.
 *
 * Neither tool is asked what to do. The control member is read for metadata
 * only, and the payload is adopted through the same checks every other backend
 * goes through, so a .deb cannot install something a tar.xz could not.
 */

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

int zbra_backend_deb_claims(const char *format, const char *filename)
{
    if (format != NULL && strcmp(format, "deb") == 0)
        return 1;

    if (filename == NULL)
        return 0;

    return strstr(filename, ".deb") != NULL;
}

int zbra_backend_deb_available(char **why)
{
    if (zbra_have_program("dpkg-deb") || zbra_have_program("ar"))
        return 1;

    if (why != NULL)
        *why = mkerr("installing .deb packages needs dpkg-deb(1) or ar(1)");

    return 0;
}

int zbra_backend_deb_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err)
{
    char           staging[4096];
    char           dir[8192];
    const char    *staging_dir;
    const char    *argv[10];
    char          *tool_err = NULL;
    int            status = 0;
    size_t         n = 0;
    const char    *base = base_name(path);

    if (c == NULL || path == NULL) {
        if (err != NULL)
            *err = mkerr("zbra_backend_deb_stage: bad arguments");
        return -1;
    }

    if (access(path, R_OK) != 0) {
        if (err != NULL)
            *err = mkerr("cannot read %s: %s", path, strerror(errno));
        return -1;
    }

    staging_dir = zbra_commit_staging(c);
    snprintf(staging, sizeof(staging), "%s/_unpack", staging_dir);
    snprintf(dir, sizeof(dir), "%s/root", staging);

    if (zbra_mkdir_p(dir, 0755) != 0) {
        if (err != NULL)
            *err = mkerr("cannot create %s: %s", dir, strerror(errno));
        return -1;
    }

    if (zbra_have_program("dpkg-deb")) {
        n = 0;
        argv[n++] = "dpkg-deb";
        argv[n++] = "--extract";
        argv[n++] = path;
        argv[n++] = dir;
        argv[n] = NULL;

        if (zbra_run_capture(argv, NULL, &tool_err, &status) != 0) {
            if (err != NULL)
                *err = tool_err != NULL ? tool_err : mkerr("cannot run dpkg-deb");
            else
                free(tool_err);
            return -1;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (err != NULL)
                *err = mkerr("cannot unpack %s: dpkg-deb failed%s%s", base,
                             tool_err != NULL ? ": " : "",
                             tool_err != NULL ? tool_err : "");
            free(tool_err);
            return -1;
        }

        free(tool_err);

        return zbra_backend_adopt(c, "_unpack/root", err);
    }

    /*
     * Fallback: ar(1) has no "extract one member into a directory" verb that
     * works everywhere, so list the archive and pull out the data member.
     */
    {
        char *listing = NULL;
        char *data_member = NULL;
        char  member[512];
        int   rc;

        n = 0;
        argv[n++] = "ar";
        argv[n++] = "t";
        argv[n++] = path;
        argv[n] = NULL;

        if (zbra_run_capture(argv, &listing, NULL, &status) != 0 || listing == NULL) {
            free(listing);
            if (err != NULL)
                *err = mkerr("cannot list %s with ar(1)", base);
            return -1;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            free(listing);
            if (err != NULL)
                *err = mkerr("%s is not a valid Debian package", base);
            return -1;
        }

        /* Data members are named data.tar.*; control ones must be ignored. */
        {
            char *line = listing;

            while (line != NULL && *line != '\0') {
                char *nl = strchr(line, '\n');

                if (nl != NULL)
                    *nl = '\0';

                if (strncmp(line, "data.tar", 8) == 0) {
                    snprintf(member, sizeof(member), "%s", line);
                    data_member = member;
                }

                line = nl != NULL ? nl + 1 : NULL;
            }
        }

        if (data_member == NULL) {
            free(listing);
            if (err != NULL)
                *err = mkerr("%s has no data member", base);
            return -1;
        }

        n = 0;
        argv[n++] = "ar";
        argv[n++] = "p";
        argv[n++] = path;
        argv[n++] = data_member;
        argv[n] = NULL;

        {
            char outfile[8192];

            snprintf(outfile, sizeof(outfile), "%s/root/data.tar", staging);

            if (zbra_run_to_file(argv, outfile, &tool_err, &status) != 0) {
                free(listing);
                if (err != NULL)
                    *err = tool_err != NULL ? tool_err
                                            : mkerr("cannot extract %s", base);
                else
                    free(tool_err);
                return -1;
            }

            free(listing);
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (err != NULL)
                *err = mkerr("ar(1) failed to extract %s from %s", data_member,
                             base);
            return -1;
        }

        n = 0;
        argv[n++] = "tar";
        argv[n++] = "--extract";
        argv[n++] = "--no-same-owner";
        argv[n++] = "--directory";
        argv[n++] = dir;
        argv[n++] = "--file";
        argv[n++] = "data.tar";
        argv[n] = NULL;

        if (zbra_run_capture(argv, NULL, &tool_err, &status) != 0 ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (err != NULL)
                *err = mkerr("cannot unpack %s: %s%s%s", base,
                             tool_err != NULL ? tool_err : "tar failed",
                             tool_err != NULL ? "" : "",
                             "");
            free(tool_err);
            return -1;
        }

        free(tool_err);

        rc = zbra_backend_adopt(c, "_unpack/root", err);

        return rc;
    }
}
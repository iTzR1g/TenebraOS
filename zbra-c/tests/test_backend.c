/*
 * Backend tests.
 *
 * The tar.xz backend is exercised for real, because it is the format
 * TenebraOS publishes and there is nothing to delegate. Every test builds its
 * own archive with tar(1), so there are no binary fixtures to keep in sync.
 *
 * The delegated backends are tested for the behaviour that matters without
 * requiring Arch or Fedora tooling on the machine running the tests: that they
 * refuse cleanly, and say something useful, when their tool is absent.
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "backend_impl.h"
#include "commit.h"
#include "db.h"
#include "index.h"

static int tests_run;
static int tests_failed;
static char g_root[256];

static void ok(int cond, const char *fmt, ...)
{
    va_list ap;

    tests_run++;
    if (cond)
        return;

    tests_failed++;
    fputs("  FAIL ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void clear_err(char **err)
{
    free(*err);
    *err = NULL;
}

static char *sub(const char *a, const char *b)
{
    static char slots[32][1024];
    static size_t next;
    char        *out = slots[next++ % 32];

    snprintf(out, sizeof(slots[0]), "%s/%s", a, b);
    return out;
}

static void put_file(const char *path, const char *content)
{
    char  parent[1024];
    char *slash;
    FILE *fp;

    snprintf(parent, sizeof(parent), "%s", path);
    slash = strrchr(parent, '/');
    if (slash != NULL) {
        *slash = '\0';
        zbra_mkdir_p(parent, 0755);
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fputs(content, fp);
    fclose(fp);
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static char *read_file(const char *path)
{
    static char buf[4096];
    FILE       *fp;
    size_t      n;

    fp = fopen(path, "r");
    if (fp == NULL)
        return NULL;

    n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    return buf;
}

/*
 * Build a tar.xz from a directory tree described as text.
 *
 * Returns the archive path, or NULL when xz is unavailable, in which case the
 * caller skips rather than fails: a test machine without xz should not be told
 * zbra is broken.
 */
static char *make_tarxz(const char *name, const char *const *files,
                        size_t nfiles, const char *wrap)
{
    char   tree[512];
    char   arch[512];
    char   cmd[2048];
    size_t i;

    /* g_root is a temp dir, so these are short; the room is for -Wformat. */
    snprintf(tree, sizeof(tree), "%s/src-%s", g_root, name);
    snprintf(arch, sizeof(arch), "%s/%s", g_root, name);

    if (zbra_mkdir_p(tree, 0755) != 0)
        return NULL;

    for (i = 0; i < nfiles; i++)
        put_file(sub(tree, files[i]), "payload\n");

    if (wrap != NULL) {
        /* Repackage under a single wrapper directory, as a release tarball is. */
        char   wrapped[512];
        char   outer[1024];
        char   wrapdir[256];

        snprintf(wrapped, sizeof(wrapped), "%s/wrapped-%s", g_root, name);
        snprintf(wrapdir, sizeof(wrapdir), "%s", wrap);
        snprintf(outer, sizeof(outer), "%s/%s", wrapped, wrapdir);
        (void)zbra_mkdir_p(outer, 0755);

        snprintf(cmd, sizeof(cmd), "cp -a '%s'/. '%s'/ 2>/dev/null", tree,
                 outer);
        if (system(cmd) != 0)
            return NULL;

        /* arch and wrapped are bounded by g_root, so this cannot overflow. */
        snprintf(cmd, sizeof(cmd),
                 "tar --create --xz --file '%s' -C '%s' '%s' 2>/dev/null",
                 arch, wrapped, wrapdir);
        if (system(cmd) != 0)
            return NULL;

        return strdup(arch);
    }

    snprintf(cmd, sizeof(cmd),
             "tar --create --xz --file '%s' -C '%s' . 2>/dev/null", arch, tree);
    if (system(cmd) != 0)
        return NULL;

    return strdup(arch);
}

/* ------------------------------------------------------------ the table */

static void test_table(void)
{
    const char *const *names = zbra_backend_names();
    size_t                i;
    int                   seen_tar = 0, seen_deb = 0, seen_rpm = 0;
    int                   seen_aur = 0, seen_snap = 0;

    printf("backends: the table covers every advertised format\n");

    ok(names != NULL && names[0] != NULL, "the table is not empty");

    for (i = 0; names[i] != NULL; i++) {
        if (strcmp(names[i], "tar.xz") == 0)
            seen_tar = 1;
        if (strcmp(names[i], "deb") == 0)
            seen_deb = 1;
        if (strcmp(names[i], "rpm") == 0)
            seen_rpm = 1;
        if (strcmp(names[i], "aur") == 0)
            seen_aur = 1;
        if (strcmp(names[i], "snap") == 0)
            seen_snap = 1;
    }

    ok(seen_tar, "tar.xz is present");
    ok(seen_deb, "deb is present");
    ok(seen_rpm, "rpm is present");
    ok(seen_aur, "aur is present");
    ok(seen_snap, "snap is present");

    ok(zbra_backend_claims("tar.xz", "tar.xz", NULL), "tar.xz claims its format");
    ok(zbra_backend_claims("tar.xz", NULL, "pkg-1.0.tar.xz"),
       "tar.xz claims by filename");
    ok(!zbra_backend_claims("tar.xz", "deb", "pkg.deb"),
       "tar.xz does not claim a .deb");
    ok(zbra_backend_claims("deb", NULL, "pkg_1.0_amd64.deb"),
       "deb claims by filename");
    ok(!zbra_backend_claims("nonsense", "deb", "pkg.deb"),
       "an unknown backend claims nothing");
    ok(!zbra_backend_claims(NULL, "deb", "pkg.deb"),
       "a NULL backend claims nothing");

    {
        char *err = NULL;

        ok(zbra_backend_available("tar.xz", &err) == 1,
           "tar.xz needs nothing but tar and xz");
        clear_err(&err);
    }
}

/* ------------------------------------------------------- tar.xz for real */

static void test_tarxz(void)
{
    static const char *files[] = { "usr/bin/tool", "usr/share/doc/tool/README" };
    char             *arch;
    zbra_commit      *c   = NULL;
    char             *err = NULL;
    zbra_package      pkg;

    printf("backends: a tar.xz package unpacks into the right layout\n");

    arch = make_tarxz("plain.tar.xz", files, 2, NULL);
    if (arch == NULL) {
        printf("  skipped (tar or xz unavailable)\n");
        return;
    }

    memset(&pkg, 0, sizeof(pkg));
    pkg.format = (char *)"tar.xz";

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened a commit");

    ok(zbra_backend_stage("tar.xz", c, arch, &pkg, &err) == 0,
       "the package stages");
    if (err != NULL)
        printf("       [err: %s]\n", err);
    clear_err(&err);

    ok(zbra_commit_count(c) == 2, "both files are staged");

    ok(zbra_commit_apply(&c, g_root, &err) == 0, "and install");
    if (err != NULL)
        printf("       [err: %s]\n", err);
    clear_err(&err);

    ok(exists(sub(g_root, "usr/bin/tool")), "the binary landed");
    ok(exists(sub(g_root, "usr/share/doc/tool/README")),
       "a nested file landed");
    ok(read_file(sub(g_root, "usr/bin/tool")) != NULL, "with its content");

    free(arch);
}

static void test_tarxz_wrapper_directory(void)
{
    static const char *files[] = { "usr/bin/wrapped" };
    char             *arch;
    zbra_commit      *c   = NULL;
    char             *err = NULL;
    zbra_package      pkg;

    printf("backends: a tarball's wrapper directory is not installed\n");

    arch = make_tarxz("wrapped.tar.xz", files, 1, "wrapped-1.0");
    if (arch == NULL) {
        printf("  skipped (tar or xz unavailable)\n");
        return;
    }

    memset(&pkg, 0, sizeof(pkg));
    pkg.format = (char *)"tar.xz";

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened a commit");
    ok(zbra_backend_stage("tar.xz", c, arch, &pkg, &err) == 0, "it stages");
    clear_err(&err);
    ok(zbra_commit_apply(&c, g_root, &err) == 0, "and installs");
    clear_err(&err);

    ok(exists(sub(g_root, "usr/bin/wrapped")),
       "the file is at its real path");
    ok(!exists(sub(g_root, "wrapped-1.0")),
       "the wrapper directory is not installed");

    free(arch);
}

static void test_tarxz_hostile(void)
{
    char        staging[1024];
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("backends: a hostile tarball cannot install outside the root\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened a commit");
    snprintf(staging, sizeof(staging), "%s", zbra_commit_staging(c));

    /* Written straight into staging, as a hostile archive would land. */
    put_file(sub(staging, "escape"), "should never be installed\n");

    /* A relative link climbing out is refused. */
    ok(symlink("../../../etc/shadow", sub(staging, "up-link")) == 0,
       "a climbing symlink is staged");
    /* An absolute link is refused. */
    ok(symlink("/etc/shadow", sub(staging, "abs-link")) == 0,
       "an absolute symlink is staged");
    /* A fifo has no business in a payload. */
    ok(mkfifo(sub(staging, "pipe"), 0644) == 0, "a fifo is staged");

    ok(zbra_backend_adopt(c, ".", &err) == -1, "adoption refuses it");
    ok(err != NULL, "the refusal explains itself");
    clear_err(&err);

    ok(zbra_commit_count(c) == 0, "nothing was recorded");

    zbra_commit_abort(c);
    ok(!exists(sub(g_root, "escape")), "and nothing was installed");
}

static void test_tarxz_symlink_inside_root_is_fine(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;
    char         staging[PATH_MAX];

    printf("backends: an ordinary relative symlink is still installed\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");
    snprintf(staging, sizeof(staging), "%s", zbra_commit_staging(c));

    put_file(sub(staging, "usr/bin/real"), "real\n");
    ok(zbra_mkdir_p(sub(staging, "usr/lib"), 0755) == 0, "made a directory");
    ok(symlink("../bin/real", sub(staging, "usr/lib/alias")) == 0,
       "staged a relative symlink within the package");

    ok(zbra_backend_adopt(c, ".", &err) == 0, "adoption accepts it");
    clear_err(&err);

    ok(zbra_commit_apply(&c, g_root, &err) == 0, "and it installs");
    clear_err(&err);

    {
        char target[1024];
        ssize_t n = readlink(sub(g_root, "usr/lib/alias"), target,
                             sizeof(target) - 1);

        ok(n > 0, "the symlink survived as a symlink");
        if (n > 0) {
            target[n] = '\0';
            ok(strcmp(target, "../bin/real") == 0, "pointing where it should");
        }
    }
}

/* --------------------------------------------------- delegated refusals */

static void test_delegated_availability(void)
{
    char *why = NULL;

    printf("backends: a delegated backend says when its tool is missing\n");

    /* Each of these must answer, whether or not the tool happens to exist. */
    ok(zbra_backend_available("deb", &why) >= 0, "deb reports availability");
    clear_err(&why);

    ok(zbra_backend_available("rpm", &why) >= 0, "rpm reports availability");
    clear_err(&why);

    ok(zbra_backend_available("aur", &why) >= 0, "aur reports availability");
    clear_err(&why);

    ok(zbra_backend_available("snap", &why) >= 0, "snap reports availability");
    clear_err(&why);

    ok(zbra_backend_available("made-up", &why) == 0,
       "an unknown backend is unavailable");
    ok(why != NULL && strstr(why, "made-up") != NULL,
       "and names itself in the complaint");
    clear_err(&why);
}

static void test_delegated_stage_refusal(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;
    zbra_package pkg;

    printf("backends: a delegated backend refuses rather than half-installing\n");

    memset(&pkg, 0, sizeof(pkg));
    pkg.name   = (char *)"somepkg";
    pkg.format = (char *)"rpm";

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");

    /*
     * A path that is not really an rpm: the backend must fail on the tool or
     * on the format, and either way must not stage anything.
     */
    put_file(sub(g_root, "not-an-rpm.rpm"), "this is not a package\n");

    if (zbra_backend_available("rpm", NULL)) {
        /* The tools exist here, so exercise the real failure path. */
        ok(zbra_backend_stage("rpm", c, sub(g_root, "not-an-rpm.rpm"), &pkg,
                              &err) == -1,
           "a bogus rpm is rejected");
        clear_err(&err);
    } else {
        ok(zbra_backend_stage("rpm", c, sub(g_root, "not-an-rpm.rpm"), &pkg,
                              &err) == -1,
           "rpm is rejected when its tools are absent");
        ok(err != NULL && strstr(err, "rpm") != NULL,
           "and the message says why");
        clear_err(&err);
    }

    ok(zbra_commit_count(c) == 0, "nothing was staged");
    zbra_commit_abort(c);
}

static void test_bad_arguments(void)
{
    char *err = NULL;

    printf("backends: bad arguments are refused\n");

    ok(zbra_backend_stage(NULL, NULL, "x", NULL, &err) == -1,
       "a NULL backend is refused");
    clear_err(&err);

    ok(zbra_backend_stage("tar.xz", NULL, "x", NULL, &err) == -1,
       "a NULL commit is refused");
    clear_err(&err);

    ok(zbra_backend_adopt(NULL, ".", &err) == -1,
       "adopting into a NULL commit is refused");
    clear_err(&err);

    ok(zbra_backend_tar_stage(NULL, NULL, NULL, &err) == -1,
       "the tar backend checks its arguments");
    clear_err(&err);

    ok(zbra_backend_deb_stage(NULL, NULL, NULL, &err) == -1,
       "the deb backend checks its arguments");
    clear_err(&err);

    ok(zbra_backend_rpm_stage(NULL, NULL, NULL, &err) == -1,
       "the rpm backend checks its arguments");
    clear_err(&err);

    ok(zbra_backend_aur_stage(NULL, NULL, NULL, &err) == -1,
       "the aur backend checks its arguments");
    clear_err(&err);

    ok(zbra_backend_snap_stage(NULL, NULL, NULL, &err) == -1,
       "the snap backend checks its arguments");
    clear_err(&err);
}

int main(void)
{
    char  tmpl[] = "/tmp/zbra-backend-XXXXXX";
    char  cmd[512];
    char *made;

    setvbuf(stdout, NULL, _IOLBF, 0);

    made = mkdtemp(tmpl);
    if (made == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s", made);

    test_table();
    test_tarxz();
    test_tarxz_wrapper_directory();
    test_tarxz_hostile();
    test_tarxz_symlink_inside_root_is_fine();
    test_delegated_availability();
    test_delegated_stage_refusal();
    test_bad_arguments();

    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    (void)system(cmd);

    printf("\nbackends: %d passed, %d failed\n", tests_run, tests_failed);
    return tests_failed != 0;
}
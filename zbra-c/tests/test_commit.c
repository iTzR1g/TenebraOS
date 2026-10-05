/*
 * Tests for the staged install path.
 *
 * The interesting cases are the ones where an install goes wrong part-way
 * through, since that is the only place a package manager can quietly damage a
 * system. Each of those gets a test that checks the filesystem afterwards, not
 * just the return code.
 */

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

static int tests_run;
static int tests_failed;
static char g_root[PATH_MAX];

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

static char *sub(const char *a, const char *b)
{
    static char slots[16][PATH_MAX];
    static size_t next;
    char        *out = slots[next++ % 16];

    snprintf(out, PATH_MAX, "%s/%s", a, b);
    return out;
}

static void put_file(const char *path, const char *content)
{
    char  parent[PATH_MAX];
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

/* Error strings are malloc'd by the caller contract; tests must free them. */
static void clear_err(char **err)
{
    free(*err);
    *err = NULL;
}

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

/* Write a file into a commit's staging directory, as a backend would. */
static int stage(zbra_commit *c, const char *rel, const char *content)
{
    put_file(sub(zbra_commit_staging(c), rel), content);
    return zbra_commit_add(c, rel, rel, NULL);
}

/* ------------------------------------------------------- a clean install */

static void test_install(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: a staged install lands where it was told\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0,
       "a commit can be opened");
    ok(err == NULL, "no error on open");

    stage(c, "usr/bin/zbra", "#!/bin/sh\n");
    stage(c, "usr/share/doc/zbra/README", "docs\n");

    ok(zbra_commit_count(c) == 2, "both files are staged");

    ok(zbra_commit_apply(&c, g_root, &err) == 0, "apply succeeds");
    ok(err == NULL, "no error on apply");
    ok(c == NULL, "a successful apply consumes the commit");

    ok(exists(sub(g_root, "usr/bin/zbra")), "the binary is in place");
    ok(exists(sub(g_root, "usr/share/doc/zbra/README")),
       "nested files get their directories");

    {
        char *body = read_file(sub(g_root, "usr/bin/zbra"));
        ok(body != NULL && strcmp(body, "#!/bin/sh\n") == 0,
           "the content survived the move");
    }

    /* Nothing may be left lying around for the next install to trip over. */
    {
        char *dbdir = sub(g_root, "db");
        char  cmd[PATH_MAX + 32];

        snprintf(cmd, sizeof(cmd), "ls %s 2>/dev/null | wc -l", dbdir);
        ok(system(cmd) == 0, "the staging tree was cleaned up");
    }
    clear_err(&err);
}


/* ------------------------------------------------------ staged, not live */

static void test_nothing_live_before_apply(void)
{
    zbra_commit *c = NULL;
    char        *err = NULL;

    printf("commit: unpacking does not touch the live filesystem\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");
    stage(c, "etc/zbra.conf", "should not exist yet\n");

    ok(!exists(sub(g_root, "etc/zbra.conf")),
       "a staged file is invisible until apply");
    ok(exists(sub(zbra_commit_staging(c), "etc/zbra.conf")),
       "but it really is in staging");

    zbra_commit_abort(c);
    ok(!exists(sub(g_root, "etc/zbra.conf")), "aborting installs nothing");
    clear_err(&err);
}


/* -------------------------------------------------------- hostile paths */

static void test_hostile_paths(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: a hostile payload cannot escape the install root\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");

    /* Each refusal overwrites *err, so clear it before the next call. */
    ok(zbra_commit_add(c, "../escape", "tmp/escape", &err) == -1,
       "a traversing payload path is refused");
    ok(err != NULL && strstr(err, "unsafe") != NULL,
       "the refusal names the problem");
    clear_err(&err);

    ok(zbra_commit_add(c, "usr/bin/ok", "../../etc/passwd", &err) == -1,
       "a traversing destination is refused");
    clear_err(&err);

    ok(zbra_commit_add(c, "/etc/passwd", "etc/passwd", &err) == -1,
       "an absolute payload path is refused");
    clear_err(&err);

    ok(zbra_commit_add(c, "usr/bin/ok", "/etc/passwd", &err) == -1,
       "an absolute destination is refused");
    clear_err(&err);

    ok(zbra_commit_count(c) == 0, "nothing hostile was recorded");
    ok(zbra_commit_apply(&c, g_root, &err) == -1,
       "a commit with nothing valid cannot be applied");

    zbra_commit_abort(c);
    clear_err(&err);
}


/* --------------------------------------------------------- preflight */

static void test_preflight_conflict(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: conflicts are found before anything is moved\n");

    put_file(sub(g_root, "var/lib/zbra/system-file"), "pre-existing\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");

    stage(c, "usr/bin/first", "new\n");
    /* A directory where a file must go: nothing there can be overwritten. */
    ok(zbra_mkdir_p(sub(g_root, "usr/lib/blocked"), 0755) == 0,
       "a directory is in the way");

    ok(zbra_commit_add(c, "usr/lib/blocked", "usr/lib/blocked", &err) == 0,
       "the clashing file is staged");

    ok(zbra_commit_apply(&c, g_root, &err) == -1, "apply refuses the clash");
    ok(err != NULL && strstr(err, "not a regular file") != NULL,
       "and explains what is in the way");

    ok(!exists(sub(g_root, "usr/bin/first")),
       "the earlier file was not moved either");

    zbra_commit_abort(c);

    {
        char *body = read_file(sub(g_root, "var/lib/zbra/system-file"));
        ok(body != NULL && strcmp(body, "pre-existing\n") == 0,
           "the untouched file is untouched");
    }
    clear_err(&err);
}


/* --------------------------------------------------------- mid-flight failure */

static void test_rollback(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: a failure part-way through is undone\n");

    /*
     * The second file's parent cannot be created because a regular file
     * occupies that name, so the move of the first file has already happened
     * by the time apply gives up. That is exactly the case where a naive
     * installer leaves half a package behind.
     */
    put_file(sub(g_root, "usr/share/collision"), "i am a file\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");

    stage(c, "usr/bin/lands-first", "first\n");
    ok(zbra_commit_add(c, "usr/share/collision/child", "usr/share/collision/child",
                       &err) == 0,
       "the second file is staged");

    ok(zbra_commit_apply(&c, g_root, &err) == -1, "apply fails");
    ok(err != NULL, "the failure is explained");

    ok(!exists(sub(g_root, "usr/bin/lands-first")),
       "the file that had already moved is put back");

    {
        char *body = read_file(sub(g_root, "usr/share/collision"));
        ok(body != NULL && strcmp(body, "i am a file\n") == 0,
           "the blocking file is still intact");
    }

    /* The commit is still usable after a failure, not left in limbo. */
    ok(zbra_commit_staging(c) != NULL, "the commit survives the failure");
    zbra_commit_abort(c);

    ok(!exists(sub(g_root, "usr/bin/lands-first")), "still nothing installed");
    clear_err(&err);
}


/* --------------------------------------------------------- replacement */

static void test_upgrade_replaces_and_keeps_original_on_failure(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: replacing a file works, and can be rewound\n");

    put_file(sub(g_root, "usr/bin/tool"), "version 1\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");
    stage(c, "usr/bin/tool", "version 2\n");

    ok(zbra_commit_apply(&c, g_root, &err) == 0, "the upgrade applies");

    {
        char *body = read_file(sub(g_root, "usr/bin/tool"));
        ok(body != NULL && strcmp(body, "version 2\n") == 0,
           "the new version is in place");
    }

    /* Now force a failure after a replacement and check the old file returns. */
    put_file(sub(g_root, "usr/lib/another-file"), "blocking\n");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "reopened");
    stage(c, "usr/bin/tool", "version 3\n");
    ok(zbra_commit_add(c, "usr/lib/another-file/x", "usr/lib/another-file/x",
                       &err) == 0,
       "the doomed file is staged");

    ok(zbra_commit_apply(&c, g_root, &err) == -1, "the upgrade fails late");

    {
        char *body = read_file(sub(g_root, "usr/bin/tool"));
        ok(body != NULL && strcmp(body, "version 2\n") == 0,
           "the displaced file was restored, not left deleted");
    }

    zbra_commit_abort(c);
    clear_err(&err);
}


/* ------------------------------------------------------------- symlinks */

static void test_symlink_destination(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: a symlink is never written through\n");

    put_file(sub(g_root, "var/lib/zbra/target"), "precious\n");
    ok(symlink(sub(g_root, "var/lib/zbra/target"),
               sub(g_root, "usr/bin/link")) == 0,
       "a symlink is in the way");

    ok(zbra_commit_open(&c, sub(g_root, "db"), &err) == 0, "opened");
    stage(c, "usr/bin/link", "overwritten\n");

    ok(zbra_commit_apply(&c, g_root, &err) == -1,
       "apply refuses to follow the symlink");

    {
        char *body = read_file(sub(g_root, "var/lib/zbra/target"));
        ok(body != NULL && strcmp(body, "precious\n") == 0,
           "the symlink target was not modified");
    }

    zbra_commit_abort(c);
    clear_err(&err);
}


/* -------------------------------------------------------------- basics */

static void test_misc(void)
{
    zbra_commit *c   = NULL;
    char        *err = NULL;

    printf("commit: argument handling\n");

    ok(zbra_commit_open(NULL, "x", &err) == -1, "a NULL commit is refused");
    clear_err(&err);

    ok(zbra_commit_open(&c, NULL, &err) == -1,
       "a NULL staging parent is refused");
    ok(err != NULL, "the caller is told why");
    clear_err(&err);

    ok(zbra_commit_open(&c, sub(g_root, "deep/nested/db"), &err) == 0,
       "a missing staging parent is created");

    ok(zbra_commit_add(c, NULL, "a", &err) == -1, "a NULL payload path");
    clear_err(&err);
    ok(zbra_commit_add(c, "a", NULL, &err) == -1, "a NULL destination");
    clear_err(&err);
    ok(zbra_commit_apply(&c, g_root, &err) == -1, "an empty commit cannot apply");

    zbra_commit_abort(c);
    ok(1, "aborting a NULL commit is harmless");
    zbra_commit_abort(NULL);

    ok(zbra_commit_staging(NULL) == NULL, "staging of NULL is NULL");
    ok(zbra_commit_count(NULL) == 0, "count of NULL is zero");
    clear_err(&err);
}


int main(void)
{
    char  tmpl[] = "/tmp/zbra-commit-XXXXXX";
    char  cmd[PATH_MAX + 64];
    char *made;

    /* Line-buffered so a crash still leaves the results on screen. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    made = mkdtemp(tmpl);
    if (made == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s", made);

    test_install();
    test_nothing_live_before_apply();
    test_hostile_paths();
    test_preflight_conflict();
    test_rollback();
    test_upgrade_replaces_and_keeps_original_on_failure();
    test_symlink_destination();
    test_misc();

    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_root);
    (void)system(cmd);

    printf("\ncommit: %d passed, %d failed\n", tests_run, tests_failed);
    return tests_failed != 0;
}
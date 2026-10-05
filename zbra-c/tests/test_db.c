/*
 * test_db.c -- unit tests for the package database.
 *
 * These tests run against a temporary root via $ZBRA_ROOT, so they never
 * touch a real installation. The path-validation cases matter most: a
 * hostile package index must not be able to make zbra write outside the
 * database or later delete an arbitrary file during removal.
 */

#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s\n", what);
    }
}

static void eq_str(const char *got, const char *want, const char *what)
{
    if (got != NULL && strcmp(got, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s: got \"%s\", want \"%s\"\n", what,
               got ? got : "(null)", want);
    }
}

static char g_base[256];
static char g_root[320];

/*
 * Each test gets its own database directory. Sharing one root would make
 * every registry count accumulate across tests, so a test asserting "one
 * explicit package" would fail because of what an earlier test inserted.
 */
static void setup_base(void)
{
    snprintf(g_base, sizeof(g_base), "/tmp/zbra-db-test-%ld", (long)getpid());
    setenv("ZBRA_ROOT", g_base, 1);
    snprintf(g_root, sizeof(g_root), "%s/base", g_base);
}

static void use_root(const char *tag)
{
    snprintf(g_root, sizeof(g_root), "%s/%s", g_base, tag);
}

static void teardown_base(void)
{
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_base);
    if (system(cmd) != 0)
        printf("  (warning: could not remove %s)\n", g_base);
}

static void mkentry(zbra_entry *e, const char *name, const char *version,
                    zbra_kind kind)
{
    memset(e, 0, sizeof(*e));
    e->name = (char *)name;
    e->version = (char *)version;
    e->kind = kind;
    e->source = (char *)"tenebraos";
    e->format = (char *)"deb";
    e->arch = (char *)"amd64";
}

/* ------------------------------------------------------------------ */

static void test_name_validation(void)
{
    puts("db: package name validation");

    ok(zbra_name_is_safe("vim"), "plain name");
    ok(zbra_name_is_safe("libc6"), "name with digit");
    ok(zbra_name_is_safe("g++"), "name with plus");
    ok(zbra_name_is_safe("python3.11"), "name with dot");
    ok(zbra_name_is_safe("libc6-dev"), "name with hyphen");
    ok(zbra_name_is_safe("foo_bar"), "name with underscore");

    /* Escapes and option-like names must be refused. */
    ok(!zbra_name_is_safe(".."), "dotdot");
    ok(!zbra_name_is_safe("../evil"), "traversal");
    ok(!zbra_name_is_safe("a/b"), "slash");
    ok(!zbra_name_is_safe("/abs"), "absolute");
    ok(!zbra_name_is_safe("-rf"), "leading dash reads as an option");
    ok(!zbra_name_is_safe(".hidden"), "leading dot");
    ok(!zbra_name_is_safe(""), "empty");
    ok(!zbra_name_is_safe(NULL), "NULL");
    ok(!zbra_name_is_safe("has space"), "space");
    ok(!zbra_name_is_safe("semi;colon"), "shell metacharacter");
    ok(!zbra_name_is_safe("back`tick"), "backtick");
    ok(!zbra_name_is_safe("dollar$"), "dollar");
}

static void test_path_validation(void)
{
    puts("db: owned-path validation");

    ok(zbra_path_is_safe("usr/bin/vim"), "normal path");
    ok(zbra_path_is_safe("usr/share/doc/x/README"), "deep path");

    ok(!zbra_path_is_safe("/usr/bin/vim"), "absolute");
    ok(!zbra_path_is_safe("../etc/passwd"), "traversal out of root");
    ok(!zbra_path_is_safe("usr/../../etc/passwd"), "embedded traversal");
    ok(!zbra_path_is_safe("./usr/bin/vim"), "leading dot-slash component");
    ok(!zbra_path_is_safe("usr//bin"), "empty component");
    ok(!zbra_path_is_safe(""), "empty");
    ok(!zbra_path_is_safe(NULL), "NULL");
    ok(!zbra_path_is_safe("usr/\x01evil"), "control character");
}

static void test_put_get(void)
{
    zbra_db db;

    use_root("putget");
    zbra_entry e, got;

    puts("db: put and get");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "vim", "9.1-2", ZBRA_KIND_PACKAGE);
    e.n_files = 0;
    e.files = NULL;
    ok(zbra_db_put(&db, &e) == 0, "put vim");

    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "vim", &got) == 1, "get vim");
    eq_str(got.name, "vim", "name roundtrip");
    eq_str(got.version, "9.1-2", "version roundtrip");
    eq_str(got.source, "tenebraos", "source roundtrip");
    eq_str(got.format, "deb", "format roundtrip");
    eq_str(got.arch, "amd64", "arch roundtrip");
    ok(got.kind == ZBRA_KIND_PACKAGE, "kind roundtrip");
    zbra_entry_free(&got);

    /* Absent names report "not found" rather than erroring. */
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "nonexistent", &got) == 0, "absent package");
    zbra_entry_free(&got);

    zbra_db_close(&db);
}

static void test_files_ownership(void)
{
    zbra_db db;

    use_root("files");
    zbra_entry e, got;
    char *files[4];

    puts("db: file ownership list");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "ed", "1.0", ZBRA_KIND_PACKAGE);
    files[0] = (char *)"usr/bin/ed";
    files[1] = (char *)"usr/share/man/man1/ed.1";
    files[2] = (char *)"usr/share/doc/ed/copyright";
    e.files = files;
    e.n_files = 3;
    ok(zbra_db_put(&db, &e) == 0, "put ed with files");

    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "ed", &got) == 1, "get ed");
    ok(got.n_files == 3, "file count roundtrip");
    if (got.n_files == 3) {
        eq_str(got.files[0], "usr/bin/ed", "file 0");
        eq_str(got.files[1], "usr/share/man/man1/ed.1", "file 1");
    }
    zbra_entry_free(&got);

    /*
     * Paths that try to escape must be dropped at write time, so a hostile
     * index cannot seed a database that later deletes /etc/passwd during
     * removal. Only the legitimate path should survive.
     */
    mkentry(&e, "evil", "1.0", ZBRA_KIND_PACKAGE);
    files[0] = (char *)"../../etc/passwd";
    files[1] = (char *)"/etc/shadow";
    files[2] = (char *)"usr/bin/evil";
    e.files = files;
    e.n_files = 3;
    ok(zbra_db_put(&db, &e) == 0, "put package with unsafe paths");

    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "evil", &got) == 1, "get evil");
    ok(got.n_files == 1, "only the safe path was recorded");
    if (got.n_files == 1)
        eq_str(got.files[0], "usr/bin/evil", "safe path kept");
    zbra_entry_free(&got);

    /* Ownership lookup must not resolve an unsafe path either. */
    {
        char *owner = zbra_db_owner_of(&db, "../../etc/passwd");
        ok(owner == NULL, "unsafe path has no owner");
        free(owner);
    }

    zbra_db_close(&db);
}

static void test_registries(void)
{
    zbra_db db;

    use_root("registries");
    zbra_entry e, got;
    size_t n = 0;
    char **list;

    puts("db: registries");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    /* Explicit package. */
    mkentry(&e, "explicit", "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == 0, "put explicit");

    /* Auto-installed dependency. */
    mkentry(&e, "autodep", "1.0", ZBRA_KIND_DEPENDENCY);
    ok(zbra_db_put(&db, &e) == 0, "put dependency");

    /* Isolated (Bedrock-style) install. */
    mkentry(&e, "isolated-pkg", "1.0", ZBRA_KIND_ISOLATED);
    e.install_root = (char *)"/opt/zbra/isolated/isolated-pkg";
    ok(zbra_db_put(&db, &e) == 0, "put isolated");

    ok(zbra_db_kind_of(&db, "explicit") == ZBRA_KIND_PACKAGE, "kind explicit");
    ok(zbra_db_kind_of(&db, "autodep") == ZBRA_KIND_DEPENDENCY, "kind dependency");
    ok(zbra_db_kind_of(&db, "isolated-pkg") == ZBRA_KIND_ISOLATED, "kind isolated");

    /* Listing is per-registry. */
    list = zbra_db_list(&db, ZBRA_KIND_PACKAGE, &n);
    ok(n == 1, "one explicit package");
    zbra_strlist_free_owned(list);

    list = zbra_db_list(&db, ZBRA_KIND_DEPENDENCY, &n);
    ok(n == 1, "one dependency");
    zbra_strlist_free_owned(list);

    list = zbra_db_list(&db, ZBRA_KIND_ISOLATED, &n);
    ok(n == 1, "one isolated");
    zbra_strlist_free_owned(list);

    list = zbra_db_list(&db, ZBRA_KIND_ANY, &n);
    ok(n == 3, "three total");
    zbra_strlist_free_owned(list);

    /* The install_root must survive the roundtrip: it is what keeps an
     * isolated package's files from colliding with the host system. */
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "isolated-pkg", &got) == 1, "get isolated");
    eq_str(got.install_root, "/opt/zbra/isolated/isolated-pkg",
           "install_root roundtrip");
    zbra_entry_free(&got);

    zbra_db_close(&db);
}

static void test_dependents(void)
{
    zbra_db db;

    use_root("dependents");
    zbra_entry e, got;

    puts("db: dependency back-references");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "libfoo", "1.0", ZBRA_KIND_DEPENDENCY);
    ok(zbra_db_put(&db, &e) == 0, "put dependency");

    mkentry(&e, "app", "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == 0, "put app");

    /* app requires libfoo. */
    ok(zbra_db_add_dependent(&db, "libfoo", "app") == 0, "add dependent");

    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "libfoo", &got) == 1, "get libfoo");
    ok(got.n_dependents == 1, "one dependent recorded");
    if (got.n_dependents == 1)
        eq_str(got.dependents[0], "app", "dependent name");
    zbra_entry_free(&got);

    /* Adding the same dependent twice must not duplicate the entry. */
    ok(zbra_db_add_dependent(&db, "libfoo", "app") == 0, "add dependent again");
    memset(&got, 0, sizeof(got));
    zbra_db_get(&db, "libfoo", &got);
    ok(got.n_dependents == 1, "dependent list is idempotent");
    zbra_entry_free(&got);

    /* Recording against something that is not installed is a no-op. */
    ok(zbra_db_add_dependent(&db, "notinstalled", "app") == 1,
       "dependent on absent package reports 1");

    zbra_db_close(&db);
}

static void test_release_dependent(void)
{
    zbra_db db;

    use_root("release");
    zbra_entry e, got;

    puts("db: releasing a dependency");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "libfoo", "1.0", ZBRA_KIND_DEPENDENCY);
    ok(zbra_db_put(&db, &e) == 0, "put dependency");
    ok(zbra_db_add_dependent(&db, "libfoo", "app") == 0, "add dependent");

    mkentry(&e, "app", "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == 0, "put app");

    /* While app still needs libfoo, releasing must NOT drop the entry. */
    ok(zbra_db_release_dependent(&db, "libfoo", "other") == 0,
       "release by a different name keeps the entry");
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "libfoo", &got) == 1, "libfoo still present");
    zbra_entry_free(&got);

    /* The real dependent goes away: now libfoo is garbage and is removed. */
    ok(zbra_db_release_dependent(&db, "libfoo", "app") == 1,
       "release last dependent removes the entry");
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "libfoo", &got) == 0, "libfoo gone");
    zbra_entry_free(&got);

    zbra_db_close(&db);
}

static void test_explicit_survives(void)
{
    zbra_db db;

    use_root("explicit");
    zbra_entry e, got;

    puts("db: explicit packages are never garbage collected");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    /* Same name and dependents situation, but explicitly installed. */
    mkentry(&e, "keepme", "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == 0, "put explicit");
    ok(zbra_db_add_dependent(&db, "keepme", "app") == 0, "add dependent");

    ok(zbra_db_release_dependent(&db, "keepme", "app") == 0, "release dependent");
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "keepme", &got) == 1,
       "explicit package kept with no dependents");
    zbra_entry_free(&got);

    zbra_db_close(&db);
}

static void test_owner_of(void)
{
    zbra_db db;

    use_root("owner");
    zbra_entry e;
    char *files[2];
    char *owner;

    puts("db: ownership lookup");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "ripgrep", "14.0", ZBRA_KIND_PACKAGE);
    files[0] = (char *)"usr/bin/rg";
    files[1] = (char *)"usr/share/man/man1/rg.1";
    e.files = files;
    e.n_files = 2;
    ok(zbra_db_put(&db, &e) == 0, "put ripgrep");

    owner = zbra_db_owner_of(&db, "usr/bin/rg");
    eq_str(owner, "ripgrep", "owner of usr/bin/rg");
    free(owner);

    owner = zbra_db_owner_of(&db, "usr/bin/does-not-exist");
    ok(owner == NULL, "no owner for unknown path");

    zbra_db_close(&db);
}

static void test_put_rejects_unsafe_name(void)
{
    zbra_db db;

    use_root("unsafe");
    zbra_entry e;
    char name[256];

    puts("db: put rejects unsafe names");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    /* A traversal name must be refused before any path is built. */
    snprintf(name, sizeof(name), "../escape");
    mkentry(&e, name, "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == -1, "traversal name refused");

    /* Confirm nothing was created outside the root. */
    {
        char probe[512];
        struct stat st;

        snprintf(probe, sizeof(probe), "%s/escape", g_base);
        ok(stat(probe, &st) != 0, "no directory escaped the db root");
    }

    zbra_db_close(&db);
}

static void test_forget(void)
{
    zbra_db db;

    use_root("forget");
    zbra_entry e, got;

    puts("db: forget");

    ok(zbra_db_open(&db, g_root) == 0, "open");

    mkentry(&e, "temporary", "1.0", ZBRA_KIND_PACKAGE);
    ok(zbra_db_put(&db, &e) == 0, "put");

    ok(zbra_db_forget(&db, "temporary", ZBRA_KIND_PACKAGE) == 1, "forget existing");
    ok(zbra_db_forget(&db, "temporary", ZBRA_KIND_PACKAGE) == 0, "forget twice");
    ok(zbra_db_forget(&db, "never-existed", ZBRA_KIND_PACKAGE) == 0,
       "forget absent");

    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "temporary", &got) == 0, "entry gone");
    zbra_entry_free(&got);

    /*
     * A forgotten entry must stop being listed. Leaving the entry directory
     * behind is enough to keep a removed package looking installed, because
     * that is exactly what listing walks.
     */
    {
        size_t n = 0;
        char **names = zbra_db_list(&db, ZBRA_KIND_ANY, &n);
        size_t i;
        int    present = 0;

        for (i = 0; names != NULL && names[i] != NULL; i++)
            if (strcmp(names[i], "temporary") == 0)
                present = 1;

        ok(!present, "a forgotten entry is not listed");
        zbra_strlist_free_owned(names);
    }

    /*
     * And a directory with no meta file in it is not an entry either, so an
     * interrupted removal cannot resurrect a package.
     */
    {
        char        orphan[4096];
        size_t      n = 0;
        char      **names;
        int         present = 0;
        size_t      i;

        snprintf(orphan, sizeof(orphan), "%s/packages/orphan", g_root);
        zbra_mkdir_p(orphan, 0755);

        names = zbra_db_list(&db, ZBRA_KIND_ANY, &n);
        for (i = 0; names != NULL && names[i] != NULL; i++)
            if (strcmp(names[i], "orphan") == 0)
                present = 1;

        ok(!present, "a directory without a meta file is not an entry");
        zbra_strlist_free_owned(names);

        rmdir(orphan);
    }

    /* Still forgettable afterwards, and re-installable. */
    ok(zbra_db_put(&db, &e) == 0, "put after forget");
    memset(&got, 0, sizeof(got));
    ok(zbra_db_get(&db, "temporary", &got) == 1, "reinstalled entry is found");
    zbra_entry_free(&got);
    ok(zbra_db_forget(&db, "temporary", ZBRA_KIND_PACKAGE) == 1,
       "forget the reinstalled entry");

    zbra_db_close(&db);
}

int main(void)
{
    setup_base();

    test_name_validation();
    test_path_validation();
    test_put_get();
    test_files_ownership();
    test_registries();
    test_dependents();
    test_release_dependent();
    test_explicit_survives();
    test_owner_of();
    test_put_rejects_unsafe_name();
    test_forget();

    teardown_base();

    printf("\ndb: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
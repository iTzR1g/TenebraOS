/*
 * test_cli.c -- the command line tool, driven as a user would.
 *
 * The unit suites cover the modules; this one covers the parts only a real
 * process can show: that the commands are wired to each other, that a package
 * survives a trip through the cache and the database, and that a hostile
 * archive does not end up outside the install root.
 *
 * Everything is redirected with ZBRA_ROOT, ZBRA_CONFIG_DIR, ZBRA_CACHE_DIR and
 * ZBRA_INSTALL_ROOT, so the suite never touches the real /var/lib/zbra. That
 * redirection is the reason those environment overrides exist, and this file is
 * what keeps them honest: if a command starts writing outside its own
 * environment, these tests start failing on the machine that runs them.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "sources.h"

static char base[1024];
static char zbra_bin[1200];
static char db_root[1200];
static char cfg_dir[1200];
static char cache_dir[1200];
static char inst_root[1200];
static char repo_dir[1200];

static int checks;
static int failures;

/* Output of the last command, and the command that produced it. */
static char last_out[16384];
static char last_cmd[4096];

static void ok(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
        printf("       command: %s\n", last_cmd);
        printf("       output: %s\n", last_out[0] != 0 ? last_out : "(nothing)");
    }
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n);

    if (p == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }

    return p;
}

/*
 * Run zbra and leave its combined output in last_out.
 *
 * system() is fine here: this is a test harness, and the point of the tool
 * under test is that *it* never uses a shell. The command text is built with
 * vsnprintf so a caller can write "sources add main tar.xz file://%s" and pass
 * the path as an argument, rather than having to assemble it at every call.
 */
static int run_zbra(const char *fmt, ...)
{
    va_list ap;
    char   *args;
    size_t  args_len = strlen(fmt) + 1024;
    size_t  need;
    char   *cmd;
    FILE   *fp;
    size_t  n = 0;

    args = xmalloc(args_len);
    va_start(ap, fmt);
    vsnprintf(args, args_len, fmt, ap);
    va_end(ap);

    need = strlen(args) + strlen(zbra_bin) + strlen(db_root) + strlen(cfg_dir) +
           strlen(cache_dir) + strlen(inst_root) + 256;
    cmd = xmalloc(need);

    snprintf(cmd, need,
             "ZBRA_ROOT='%s' ZBRA_CONFIG_DIR='%s' ZBRA_CACHE_DIR='%s' "
             "ZBRA_INSTALL_ROOT='%s' '%s' %s 2>&1",
             db_root, cfg_dir, cache_dir, inst_root, zbra_bin, args);

    snprintf(last_cmd, sizeof(last_cmd), "zbra %s", args);
    last_out[0] = 0;

    fp = popen(cmd, "r");
    if (fp == NULL) {
        fprintf(stderr, "cannot run %s: %s\n", cmd, strerror(errno));
        exit(2);
    }

    while (1) {
        char   chunk[1024];
        size_t got = fread(chunk, 1, sizeof(chunk) - 1, fp);

        if (got == 0)
            break;
        chunk[got] = 0;

        if (n + got + 1 < sizeof(last_out)) {
            memcpy(last_out + n, chunk, got + 1);
            n += got;
        }
    }

    free(cmd);
    free(args);

    return pclose(fp) == 0 ? 0 : 1;
}

static int write_file(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");

    if (fp == NULL)
        return -1;

    fputs(text, fp);

    return fclose(fp) == 0 ? 0 : -1;
}

static int path_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* Build <repo>/<file> as a tar.xz holding usr/bin/<bin_name> and its doc. */
static void make_tarball(const char *file, const char *bin_name,
                         const char *body)
{
    char stage[2048];
    char path[2048];
    char cmd[4096];

    snprintf(stage, sizeof(stage), "%s/stage-%s", base, file);
    mkdir(stage, 0755);

    /* mkdir is not recursive, so each level is created in turn. */
    snprintf(path, sizeof(path), "%s/usr", stage);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/usr/bin", stage);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/usr/share", stage);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/usr/share/doc", stage);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/usr/share/doc/%s", stage, bin_name);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/usr/bin/%s", stage, bin_name);
    if (write_file(path, body) != 0) {
        fprintf(stderr, "cannot write the fixture payload\n");
        exit(2);
    }

    snprintf(path, sizeof(path), "%s/usr/share/doc/%s/README", stage, bin_name);
    if (write_file(path, "readme\n") != 0) {
        fprintf(stderr, "cannot write the fixture doc\n");
        exit(2);
    }

    snprintf(cmd, sizeof(cmd), "tar --create --xz --file '%s/%s' -C '%s' .",
             repo_dir, file, stage);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot build the fixture %s\n", file);
        exit(2);
    }
}

/* An archive whose only member tries to climb out of the install root. */
static void make_evil_tarball(const char *file)
{
    char cmd[4096];

    write_file("/tmp/zbra-test-evil-payload", "pwned\n");

    snprintf(cmd, sizeof(cmd),
             "tar --create --xz --file '%s/%s' -C /tmp "
             "--transform 's|^zbra-test-evil-payload|../../tmp/escaped|' "
             "zbra-test-evil-payload 2>/dev/null", repo_dir, file);
    if (system(cmd) != 0) {
        fprintf(stderr, "cannot build the hostile fixture\n");
        exit(2);
    }

    unlink("/tmp/zbra-test-evil-payload");
}

/* Write an index by hand, for records a file name cannot express. */
static int write_index(const char *json)
{
    char  path[1300];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/index.json", repo_dir);

    fp = fopen(path, "w");
    if (fp == NULL)
        return -1;

    fputs(json, fp);

    return fclose(fp) == 0 ? 0 : -1;
}

/*
 * Where the zbra binary is.
 *
 * The runner lives in tests/, so the binary is one directory up. Deriving it
 * from argv[0] rather than hard-coding a path keeps `make test` working from
 * the top of the tree and from a sanitizer build in a temporary directory.
 */
static void find_zbra(const char *argv0)
{
    const char *env = getenv("ZBRA_TEST_BIN");
    char        buf[1024];
    char       *slash;

    if (env != NULL && env[0] != 0) {
        snprintf(zbra_bin, sizeof(zbra_bin), "%s", env);
        return;
    }

    snprintf(buf, sizeof(buf), "%s", argv0);
    slash = strrchr(buf, '/');
    if (slash != NULL) {
        *slash = 0;
        slash = strrchr(buf, '/');
    }

    if (slash != NULL) {
        *slash = 0;
        snprintf(zbra_bin, sizeof(zbra_bin), "%s/zbra", buf);
    } else {
        snprintf(zbra_bin, sizeof(zbra_bin), "./zbra");
    }
}

static void setup_paths(void)
{
    /*
     * Per-process so two runs cannot collide, and under /tmp so the suite needs
     * no write access anywhere real.
     */
    snprintf(base, sizeof(base), "/tmp/zbra-cli-test-%ld", (long)getpid());
    snprintf(db_root, sizeof(db_root), "%s/db", base);
    snprintf(cfg_dir, sizeof(cfg_dir), "%s/etc", base);
    snprintf(cache_dir, sizeof(cache_dir), "%s/cache", base);
    snprintf(inst_root, sizeof(inst_root), "%s/root", base);
    snprintf(repo_dir, sizeof(repo_dir), "%s/repo", base);

    if (mkdir(base, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s: %s\n", base, strerror(errno));
        exit(2);
    }

    mkdir(db_root, 0755);
    mkdir(cfg_dir, 0755);
    mkdir(cache_dir, 0755);
    mkdir(repo_dir, 0755);
    mkdir(inst_root, 0755);
}

static void cleanup(void)
{
    char cmd[4096];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", base);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: could not clean up %s\n", base);
}

/* Every test that runs update needs a source pointing at the fixture repo. */
static void add_repo_source(void)
{
    ok(run_zbra("sources add main tar.xz file://%s", repo_dir) == 0,
       "the repository source is configured");
}

static void test_sources_lifecycle(void)
{
    puts("cli: source configuration");

    ok(run_zbra("sources add main tar.xz file://%s", repo_dir) == 0,
       "adding a source succeeds");
    ok(strstr(last_out, "main") != NULL, "adding a source names it");

    ok(run_zbra("sources") == 0, "listing sources succeeds");
    ok(strstr(last_out, "main") != NULL, "the source is listed");
    ok(strstr(last_out, "tar.xz") != NULL, "the source type is shown");

    /* The configuration has to survive a re-read, or update sees nothing. */
    {
        zbra_sources s;

        zbra_sources_init(&s);
        ok(zbra_sources_load(&s, cfg_dir, NULL, NULL) == 0,
           "the written configuration parses");
        ok(s.n == 1 && s.items[0].id != NULL &&
           strcmp(s.items[0].id, "main") == 0,
           "the source id round-trips through the file");
        zbra_sources_free(&s);
    }

    ok(run_zbra("sources remove main") == 0, "removing a source succeeds");
    ok(run_zbra("sources") == 0, "listing again succeeds");
    ok(strstr(last_out, "main") == NULL, "the source is gone");
    ok(run_zbra("sources remove main") != 0,
       "removing an unknown source is refused");
}

static void test_update_and_search(void)
{
    char path[1300];

    puts("cli: update and search");

    make_tarball("hello-1.0.tar.xz", "hello", "#!/bin/sh\necho hello\n");

    snprintf(path, sizeof(path), "%s/hello-1.0.tar.xz", repo_dir);
    ok(path_exists(path), "the fixture archive was built");

    add_repo_source();

    ok(run_zbra("update") == 0, "update succeeds");
    ok(strstr(last_out, "1 packages") != NULL,
       "update finds the package in the repository");

    snprintf(path, sizeof(path), "%s/index.json", cache_dir);
    ok(path_exists(path), "update wrote a cached index");

    ok(run_zbra("search hello") == 0, "search succeeds");
    ok(strstr(last_out, "hello") != NULL, "search finds the package");
    ok(strstr(last_out, "1.0") != NULL, "search shows the version");

    /* An empty result is a normal answer, not an error. */
    ok(run_zbra("search zzz-nothing-here") == 0,
       "searching for an absent name succeeds");
    ok(strstr(last_out, "Nothing matches") != NULL,
       "searching for an absent name says so");
    ok(strstr(last_out, "hello") == NULL,
       "searching for an absent name lists nothing");
}

static void test_install_verify_remove(void)
{
    char path[1300];

    puts("cli: install, verify and remove");

    add_repo_source();
    ok(run_zbra("update") == 0, "update succeeds");

    ok(run_zbra("install hello") == 0, "install succeeds");
    ok(strstr(last_out, "Installed hello 1.0") != NULL,
       "install reports success");

    snprintf(path, sizeof(path), "%s/usr/bin/hello", inst_root);
    ok(path_exists(path), "the executable landed in the install root");
    snprintf(path, sizeof(path), "%s/usr/share/doc/hello/README", inst_root);
    ok(path_exists(path), "the documentation landed too");

    snprintf(path, sizeof(path), "%s/hello-1.0.tar.xz", cache_dir);
    ok(path_exists(path), "the payload was cached");

    ok(run_zbra("list") == 0, "list succeeds");
    ok(strstr(last_out, "hello") != NULL,
       "the package is listed as installed");

    ok(run_zbra("show hello") == 0, "show succeeds");
    ok(strstr(last_out, "1.0") != NULL, "show prints the version");

    /*
     * Removing and re-installing is the test that the database records files as
     * well as names: without a manifest, remove reports zero files and the
     * install root quietly accumulates garbage.
     */
    ok(run_zbra("remove hello") == 0, "remove succeeds");
    ok(strstr(last_out, "2 file") != NULL,
       "remove reports the files it removed");

    snprintf(path, sizeof(path), "%s/usr/bin/hello", inst_root);
    ok(!path_exists(path), "the executable is gone");
    snprintf(path, sizeof(path), "%s/usr/share/doc/hello/README", inst_root);
    ok(!path_exists(path), "the documentation is gone");

    ok(run_zbra("list") == 0, "list succeeds after removal");
    ok(strstr(last_out, "hello") == NULL, "the package is no longer listed");

    ok(run_zbra("verify") == 0, "verify succeeds");
    ok(strstr(last_out, "checks out") != NULL, "verify is clean after removal");

    /* And a second time round, this time served from the cache. */
    ok(run_zbra("install hello") == 0, "re-install succeeds");
    ok(run_zbra("verify") == 0, "verify is clean after re-install");
    ok(strstr(last_out, "checks out") != NULL,
       "the re-installed package is sound");

    ok(run_zbra("remove hello") == 0, "remove succeeds again");
}

static void test_dependency_chain(void)
{
    char json[4096];
    char path[1300];

    puts("cli: dependencies");

    /*
     * libhello is required by apphello. The index is hand-written because the
     * directory scan cannot express a requirement -- a file name carries no
     * dependencies -- and without this case the resolver is never exercised at
     * all by the command line tests.
     */
    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"apphello\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"apphello-1.0.tar.xz\",\n"
             "      \"depends\": [\"libhello (>= 1.0)\"]\n"
             "    },\n"
             "    {\n"
             "      \"name\": \"libhello\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"libhello-1.0.tar.xz\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n", repo_dir, repo_dir);

    ok(write_index(json) == 0, "the index fixture was written");

    make_tarball("apphello-1.0.tar.xz", "apphello", "#!/bin/sh\n");
    make_tarball("libhello-1.0.tar.xz", "libhello", "#!/bin/sh\n");

    /*
     * The repository holds an index.json as well as the archives, so the source
     * has to point at the index rather than at the directory: a directory is
     * scanned for file names, and a file name carries no dependencies.
     */
    ok(run_zbra("sources remove main") == 0, "the directory source is dropped");
    ok(run_zbra("sources add main tar.xz file://%s/index.json", repo_dir) == 0,
       "the index is configured as its own source");
    ok(run_zbra("update") == 0, "update succeeds");

    ok(run_zbra("install apphello") == 0,
       "installing a package with a dependency succeeds");
    ok(strstr(last_out, "Plan (2 package") != NULL,
       "the plan includes the dependency");
    ok(strstr(last_out, "libhello") != NULL, "the plan names the dependency");

    snprintf(path, sizeof(path), "%s/usr/bin/libhello", inst_root);
    ok(path_exists(path), "the dependency was installed");

    /* The dependency has to come first, or the install order is wrong. */
    {
        const char *lib = strstr(last_out, "libhello 1.0");
        const char *app = strstr(last_out, "apphello 1.0");

        ok(lib != NULL && app != NULL && lib < app,
           "the dependency is installed before the package needing it");
    }

    ok(run_zbra("verify") == 0, "verify succeeds");
    ok(strstr(last_out, "checks out") != NULL,
       "the dependency is recorded as satisfied");

    ok(run_zbra("remove apphello") == 0, "removing the package succeeds");
}

static void test_unmet_requirement(void)
{
    puts("cli: unmet requirements");

    /* apphello needs libhello, which is neither installed nor offered now. */
    ok(run_zbra("remove libhello") == 0, "removing the dependency succeeds");

    {
        char json[2048];

        snprintf(json, sizeof(json),
                 "{\n  \"format\": 1,\n  \"packages\": [\n"
                 "    {\n"
                 "      \"name\": \"apphello\",\n"
                 "      \"version\": \"1.0\",\n"
                 "      \"format\": \"tar.xz\",\n"
                 "      \"source_id\": \"main\",\n"
                 "      \"source_url\": \"file://%s\",\n"
                 "      \"filename\": \"apphello-1.0.tar.xz\",\n"
                 "      \"depends\": [\"libmissing (>= 2.0)\"]\n"
                 "    }\n"
                 "  ]\n}\n", repo_dir);
        ok(write_index(json) == 0, "the index fixture was rewritten");
    }

    ok(run_zbra("update") == 0, "update succeeds");
    (void)0;

    ok(run_zbra("install apphello") != 0,
       "installing with an unsatisfiable requirement fails");
    ok(strstr(last_out, "libmissing") != NULL,
       "the failure names the missing requirement");

}

static void test_hostile_payload(void)
{
    puts("cli: hostile payload");

    make_evil_tarball("evil-1.0.tar.xz");

    {
        char json[2048];

        snprintf(json, sizeof(json),
                 "{\n  \"format\": 1,\n  \"packages\": [\n"
                 "    {\n"
                 "      \"name\": \"evil\",\n"
                 "      \"version\": \"1.0\",\n"
                 "      \"format\": \"tar.xz\",\n"
                 "      \"source_id\": \"main\",\n"
                 "      \"source_url\": \"file://%s\",\n"
                 "      \"filename\": \"evil-1.0.tar.xz\",\n"
                 "      \"depends\": []\n"
                 "    }\n"
                 "  ]\n}\n", repo_dir);
        ok(write_index(json) == 0, "the hostile index fixture was written");
    }

    ok(run_zbra("update") == 0, "update succeeds");
    (void)0;

    /*
     * Whether the install is refused or the traversal is stripped, the one
     * thing that must never happen is a write outside the install root.
     */
    run_zbra("install evil");

    ok(!path_exists("/tmp/escaped"),
       "a traversal in the archive did not reach /tmp");
    ok(!path_exists("/tmp/zbra-cli-test/escaped"), "nothing escaped elsewhere");

    ok(run_zbra("list") == 0, "the tool still works afterwards");
    ok(run_zbra("verify") == 0, "verify still works afterwards");
}

static void test_rejects_bad_names(void)
{
    puts("cli: rejected input");

    ok(run_zbra("install ../../etc/shadow") != 0,
       "a traversal as a package name is refused");
    ok(run_zbra("install /etc/shadow") != 0,
       "an absolute path as a package name is refused");
    ok(run_zbra("install") != 0, "install without a name is refused");
    ok(run_zbra("nonsense") != 0, "an unknown command fails");

    ok(run_zbra("--version") == 0, "--version works");
    ok(strstr(last_out, "zbra") != NULL, "--version names the program");
    ok(run_zbra("--help") == 0, "--help works");
}

static void test_hand_written_sources(void)
{
    char path[1300];

    puts("cli: sources.list written by hand");

    /*
     * A user editing /etc/zbra/sources.list directly is the normal case, and it
     * bypasses `sources add` entirely, so it gets its own case.
     */
    snprintf(path, sizeof(path), "%s/sources.list", cfg_dir);
    ok(write_file(path,
                  "# a comment\n"
                  "manual deb file:///nowhere\n"
                  "bogus notatype file:///elsewhere\n") == 0,
       "a hand-written sources.list can be written");

    {
        zbra_sources s;
        char       *warnings = NULL;

        zbra_sources_init(&s);
        ok(zbra_sources_load(&s, cfg_dir, NULL, &warnings) == 0,
           "an unknown source type does not stop the load");
        ok(s.n == 1, "only the valid source is kept");
        ok(warnings != NULL && strstr(warnings, "notatype") != NULL,
           "the skipped source is reported");
        free(warnings);
        zbra_sources_free(&s);
    }

    ok(run_zbra("sources") == 0, "sources works with a hand-written file");
    ok(strstr(last_out, "manual") != NULL, "the hand-written source is listed");

    /* update has to survive a source it cannot reach. */
    ok(run_zbra("update") != 0 || strstr(last_out, "failed") != NULL,
       "update reports the source it could not read");
}

int main(int argc, char **argv)
{
    (void)argc;

    puts("cli: end-to-end command line behaviour");

    find_zbra(argv[0]);
    setup_paths();

    test_sources_lifecycle();
    test_update_and_search();
    test_install_verify_remove();
    test_dependency_chain();
    test_unmet_requirement();
    test_hostile_payload();
    test_rejects_bad_names();
    test_hand_written_sources();

    cleanup();

    printf("cli: %d passed, %d failed\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
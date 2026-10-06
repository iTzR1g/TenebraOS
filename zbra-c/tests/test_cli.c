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
#include "sha256.h"
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
/*
 * Join a directory and a relative path into a buffer big enough for both.
 *
 * Every fixture builds paths the same way, and doing it through one function
 * means the bound is a constant rather than something each call site has to
 * re-argue for the compiler. The return value is the buffer, so a caller that
 * ignores it still gets the composed path.
 */
static char *path_join(char *buf, size_t len, const char *dir, const char *rel)
{
    int n = snprintf(buf, len, "%s/%s", dir, rel);

    /*
     * A truncated path in a test fixture would silently point somewhere else
     * and turn a real failure into a confusing one, so it is worth a hard stop.
     */
    if (n < 0 || (size_t)n >= len) {
        fprintf(stderr, "fixture path too long: %s/%s\n", dir, rel);
        exit(2);
    }

    return buf;
}

static void make_tarball(const char *file, const char *bin_name,
                         const char *body)
{
    char stage[4096];
    char path[8192];
    char cmd[8192];

    snprintf(stage, sizeof(stage), "%s/stage-%s", base, file);
    mkdir(stage, 0755);

    /* mkdir is not recursive, so each level is created in turn. */
    mkdir(path_join(path, sizeof(path), stage, "usr"), 0755);
    mkdir(path_join(path, sizeof(path), stage, "usr/bin"), 0755);
    mkdir(path_join(path, sizeof(path), stage, "usr/share"), 0755);
    mkdir(path_join(path, sizeof(path), stage, "usr/share/doc"), 0755);
    mkdir(path_join(path, sizeof(path), stage, "usr/share/doc"), 0755);
    {
        char rel[256];

        snprintf(rel, sizeof(rel), "usr/share/doc/%s", bin_name);
        mkdir(path_join(path, sizeof(path), stage, rel), 0755);
        snprintf(rel, sizeof(rel), "usr/bin/%s", bin_name);
        path_join(path, sizeof(path), stage, rel);
    }
    if (write_file(path, body) != 0) {
        fprintf(stderr, "cannot write the fixture payload\n");
        exit(2);
    }

    {
        char rel[256];

        snprintf(rel, sizeof(rel), "usr/share/doc/%s/README", bin_name);
        path_join(path, sizeof(path), stage, rel);
    }

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

/*
 * The "sha256:<hex>" a repository would publish for one of its payloads.
 *
 * Built from the file that was actually written, so the fixture describes what
 * is really there. Using the module under test to generate the expectation is
 * fine here: test_sha256.c pins the hash against published vectors, and what
 * these tests are checking is that install verifies and refuses, not that the
 * arithmetic is right.
 */
static const char *payload_sum(const char *file)
{
    static char sums[8][96];
    static size_t slot;
    char         *out = sums[slot++ % 8];
    char          path[8192];

    path_join(path, sizeof(path), repo_dir, file);

    if (zbra_sha256_file(path, out) != 0) {
        fprintf(stderr, "cannot digest %s\n", path);
        exit(2);
    }

    /* Prepend the algorithm label in place, in the wide buffer. */
    memmove(out + 7, out, ZBRA_SHA256_HEX_LEN + 1);
    memcpy(out, "sha256:", 7);

    return out;
}

/*
 * A digest that is well-formed and certainly wrong: the last hex digit of a
 * real digest, nudged. Used to prove that a payload which does not match is
 * refused rather than installed and noticed later.
 */
static const char *payload_sum_wrong(const char *file)
{
    static char buf[96];
    const char *good = payload_sum(file);

    snprintf(buf, sizeof(buf), "%s", good);
    buf[ZBRA_SHA256_HEX_LEN - 1] = buf[ZBRA_SHA256_HEX_LEN - 1] == '0' ? '1' : '0';

    return buf;
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
    /*
     * The archives come first: the index records their digests, so it cannot be
     * written until they exist.
     */
    make_tarball("apphello-1.0.tar.xz", "apphello", "#!/bin/sh\n");
    make_tarball("libhello-1.0.tar.xz", "libhello", "#!/bin/sh\n");

    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"apphello\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"apphello-1.0.tar.xz\",\n"
             "      \"checksum\": \"%s\",\n"
             "      \"depends\": [\"libhello (>= 1.0)\"]\n"
             "    },\n"
             "    {\n"
             "      \"name\": \"libhello\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"libhello-1.0.tar.xz\",\n"
             "      \"checksum\": \"%s\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n",
             repo_dir, payload_sum("apphello-1.0.tar.xz"),
             repo_dir, payload_sum("libhello-1.0.tar.xz"));

    ok(write_index(json) == 0, "the index fixture was written");

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

    /*
     * The requirement is recorded against the installed package, so verify has
     * something to check that is not "ask the repository again".
     */
    {
        zbra_db     db;
        zbra_entry  e;
        size_t      i;

        ok(zbra_db_open(&db, db_root) == 0, "the database opens");
        memset(&e, 0, sizeof(e));
        ok(zbra_db_get(&db, "apphello", &e) == 1, "apphello is recorded");

        for (i = 0; i < e.n_depends; i++)
            if (e.depends[i] != NULL &&
                strcmp(e.depends[i], "libhello (>= 1.0)") == 0)
                break;

        ok(i < e.n_depends,
           "the requirement is stored, constraint and all");

        /*
         * And the dependency knows it is needed. This is the link that used to
         * be dropped: the raw text "libhello (>= 1.0)" was handed to a name
         * lookup, which never matched, so nothing was ever protected and the
         * library could be deleted out from under the program.
         */
        {
            zbra_entry lib;
            size_t     j;
            int        seen = 0;

            memset(&lib, 0, sizeof(lib));
            ok(zbra_db_get(&db, "libhello", &lib) == 1, "libhello is recorded");

            for (j = 0; j < lib.n_dependents; j++)
                if (lib.dependents[j] != NULL &&
                    strcmp(lib.dependents[j], "apphello") == 0)
                    seen = 1;

            ok(seen, "libhello knows that apphello needs it");
            zbra_entry_free(&lib);
        }

        zbra_entry_free(&e);
        zbra_db_close(&db);
    }

    /* Removing a dependency that something still needs is refused. */
    ok(run_zbra("remove libhello") != 0,
       "removing a dependency that is still needed is refused");
    snprintf(path, sizeof(path), "%s/usr/bin/libhello", inst_root);
    ok(path_exists(path), "the refused dependency is still on disk");

    ok(run_zbra("remove apphello") == 0, "removing the package succeeds");

    /* With the dependent gone, the dependency can be collected. */
    ok(run_zbra("remove libhello") == 0,
       "removing the dependency succeeds once nothing needs it");
    snprintf(path, sizeof(path), "%s/usr/bin/libhello", inst_root);
    ok(!path_exists(path), "the dependency is gone");
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
                 "      \"checksum\": \"%s\",\n"
                 "      \"depends\": [\"libmissing (>= 2.0)\"]\n"
                 "    }\n"
                 "  ]\n}\n", repo_dir,
                 payload_sum("apphello-1.0.tar.xz"));
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
                 "      \"checksum\": \"%s\",\n"
                 "      \"depends\": []\n"
                 "    }\n"
                 "  ]\n}\n", repo_dir, payload_sum("evil-1.0.tar.xz"));
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

/*
 * An index checksum is a promise, so a payload that does not match it must not
 * reach the install root -- and nothing about the failure may leave a usable
 * cache entry behind.
 *
 * The mismatch is produced by publishing a digest for one file while the source
 * holds another. That is the interesting case: the index is well-formed, the
 * file is readable, the format is right, and the only thing standing between
 * those bytes and / is the checksum.
 */
static void test_checksum_enforcement(void)
{
    char json[2600];
    char path[1300];

    puts("cli: checksums are enforced");

    make_tarball("summed-1.0.tar.xz", "summed", "#!/bin/sh\n");

    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"summed\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"summed-1.0.tar.xz\",\n"
             "      \"checksum\": \"%s\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n", repo_dir, payload_sum_wrong("summed-1.0.tar.xz"));

    ok(write_index(json) == 0, "an index with a wrong digest is written");
    ok(run_zbra("update") == 0, "update accepts the index");

    ok(run_zbra("install summed") != 0, "installing a mismatched payload fails");
    ok(strstr(last_out, "does not match") != NULL,
       "the failure says the payload does not match the index");

    path_join(path, sizeof(path), inst_root, "usr/bin/summed");
    ok(!path_exists(path), "nothing was installed");

    ok(run_zbra("list") == 0, "list works after the refusal");
    ok(strstr(last_out, "summed") == NULL,
       "the refused package is not recorded as installed");

    /*
     * The bad bytes must not be left in the cache. If they were, the next
     * attempt would find the file already present and take it on trust, which
     * would turn a caught error into a silent install.
     */
    path_join(path, sizeof(path), cache_dir, "summed-1.0.tar.xz");
    ok(!path_exists(path), "the mismatched payload is not left in the cache");

    /* And the same file with the digest it actually has installs. */
    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"summed\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"summed-1.0.tar.xz\",\n"
             "      \"checksum\": \"%s\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n", repo_dir, payload_sum("summed-1.0.tar.xz"));

    ok(write_index(json) == 0, "the index is corrected");
    ok(run_zbra("update") == 0, "update reads the corrected index");
    ok(run_zbra("install summed") == 0,
       "the payload installs once the index tells the truth");
    ok(run_zbra("remove summed") == 0, "and it can be removed again");

    /* An entry with no checksum at all is refused, not waved through. */
    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"summed\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"summed-1.0.tar.xz\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n", repo_dir);

    ok(write_index(json) == 0, "an index with no checksum is written");
    ok(run_zbra("update") == 0, "update accepts it");
    ok(run_zbra("install summed") != 0,
       "a payload the index cannot vouch for is refused");
    ok(strstr(last_out, "checksum") != NULL,
       "the refusal names the missing checksum");

    /* A checksum in a form zbra cannot compute is not a pass either. */
    snprintf(json, sizeof(json),
             "{\n  \"format\": 1,\n  \"packages\": [\n"
             "    {\n"
             "      \"name\": \"summed\",\n"
             "      \"version\": \"1.0\",\n"
             "      \"format\": \"tar.xz\",\n"
             "      \"source_id\": \"main\",\n"
             "      \"source_url\": \"file://%s\",\n"
             "      \"filename\": \"summed-1.0.tar.xz\",\n"
             "      \"checksum\": \"md5:0123456789abcdef0123456789abcdef\",\n"
             "      \"depends\": []\n"
             "    }\n"
             "  ]\n}\n", repo_dir);

    ok(write_index(json) == 0, "an index with an md5 checksum is written");
    ok(run_zbra("update") == 0, "update accepts it");
    ok(run_zbra("install summed") != 0,
       "a checksum zbra cannot compute is refused rather than ignored");
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
    test_checksum_enforcement();
    test_rejects_bad_names();
    test_hand_written_sources();

    cleanup();

    printf("cli: %d passed, %d failed\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
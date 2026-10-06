/*
 * test_index.c -- unit tests for the unified package index.
 *
 * The security-relevant cases are grouped at the end under "hostile index".
 * An index is remote input that decides which packages exist and which files
 * they claim to own, so the parser's job is not only to be correct but to
 * refuse anything that looks like an attempt to escape the install root.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db.h"
#include "deb822.h"
#include "deps.h"
#include "index.h"
#include "sha256.h"

static int tests_run;
static int tests_failed;

/*
 * Evaluate `cond` exactly ONCE.
 *
 * An earlier version of this macro used `cond` twice -- once to choose the
 * label and once in the failure test -- which silently ran any call with side
 * effects a second time. It made a two-stanza index appear to contain four
 * records. Bind the result to a local first so that cannot recur.
 */
#define ok(cond, ...)                                                      \
    do {                                                                   \
        int ok_res = (cond) ? 1 : 0;                                      \
        tests_run++;                                                       \
        printf(ok_res ? "  ok   " : "  FAIL ");                           \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                      \
        if (!ok_res)                                                       \
            tests_failed++;                                                \
    } while (0)

static char g_dir[512];

static void mktemp_dir(const char *label)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/zbra-idx-test/%s-%d", label,
             (int)getpid());
    mkdir("/tmp/zbra-idx-test", 0755);
    mkdir(g_dir, 0755);
}

/*
 * Write a fixture file and return its path.
 *
 * A ring of buffers, not one: callers legitimately hold two paths at once
 * (for a merge test, say), and a single static buffer made both pointers
 * alias, so both reads ended up loading the same file.
 */
static char *write_tmp(const char *name, const char *content)
{
    static char   slots[8][1024];
    static size_t next_slot;
    size_t        slot = next_slot++ % 8;
    char         *path = slots[slot];
    FILE         *fp;

    snprintf(path, sizeof(slots[0]), "%s/%s", g_dir, name);
    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fputs(content, fp);
    fclose(fp);

    return path;
}

/* ------------------------------------------------------- deb Packages */

static const char *DEB_SAMPLE =
    "Package: tenebra-branding\n"
    "Version: 1.2-3\n"
    "Architecture: all\n"
    "Maintainer: TenebraOS <team@tenebra.org>\n"
    "Installed-Size: 1024\n"
    "Depends: fonts-dejavu-core (>= 2.37), libc6\n"
    "Provides: tenebra-theme\n"
    "Conflicts: other-branding\n"
    "Filename: pool/tenebra-branding_1.2-3_all.deb\n"
    "Size: 32768\n"
    "SHA256: abc123\n"
    "Description: TenebraOS branding assets\n"
    " This is the second line of a folded description.\n"
    "Homepage: https://tenebra.org\n"
    "\n"
    "Package: tenebra-kernel\n"
    "Version: 6.12.0\n"
    "Architecture: amd64\n"
    "Depends: linux-base\n"
    "Filename: pool/tenebra-kernel_6.12.0_amd64.deb\n"
    "Size: 1048576\n"
    "SHA256: def456\n"
    "Description: TenebraOS kernel\n"
    "\n";

static void test_deb_packages(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: Debian Packages parsing");

    mktemp_dir("deb");
    path = write_tmp("Packages", DEB_SAMPLE);

    zbra_index_init(&idx);
    ok(zbra_index_read_deb_packages(&idx, path, "tenebra",
                                    "https://example.org/", &err) == 0,
       "Packages file parses");
    ok(idx.n == 2, "two stanzas indexed");

    {
        zbra_package p;
        int found = zbra_index_find(&idx, "tenebra-branding", &p);

        ok(found == 1, "branding package found");

        /*
         * Guard the rest: dereferencing a struct that find() did not fill
         * turns a readable failure into a segfault that hides the cause.
         */
        if (found != 1) {
            printf("  (skipping field checks; package was not found)\n");
        } else {
        ok(strcmp(p.name, "tenebra-branding") == 0, "name");
        ok(strcmp(p.version, "1.2-3") == 0, "version");
        ok(strcmp(p.arch, "all") == 0, "architecture");
        ok(strcmp(p.format, "deb") == 0, "format is deb");
        ok(p.style == ZBRA_VER_DEB, "version style is Debian");
        ok(strcmp(p.source_id, "tenebra") == 0, "source id recorded");
        ok(p.size == 32768, "size parsed");
        ok(p.checksum != NULL && strncmp(p.checksum, "sha256:", 7) == 0,
           "checksum is scheme-prefixed");
        ok(p.n_depends == 2, "both dependencies captured");
        ok(p.n_provides == 1, "provides captured");
        ok(p.n_conflicts == 1, "conflicts captured");
        ok(p.homepage != NULL && strcmp(p.homepage, "https://tenebra.org") == 0,
           "homepage captured");

        /*
         * The folded continuation must be rejoined, and must not bleed into
         * the next stanza -- that bug produces a description ending in
         * "Package: tenebra-kernel".
         */
        ok(p.description != NULL &&
           strstr(p.description, "second line") != NULL &&
           strstr(p.description, "tenebra-kernel") == NULL,
           "folded description is rejoined without swallowing the next stanza");
        }
    }

    ok(idx.n_warnings == 0, "a clean file produces no warnings");

    zbra_index_free(&idx);
    free(err);
}

static void test_deb_missing_file(void)
{
    zbra_index idx;
    char      *err = NULL;

    puts("index: a missing index file is an error with a reason");

    mktemp_dir("missing");

    zbra_index_init(&idx);
    ok(zbra_index_read_deb_packages(&idx, "/nonexistent/Packages", "s",
                                    "u", &err) == -1, "open fails");
    ok(err != NULL && strstr(err, "No such file") != NULL,
       "the message names the real cause");
    free(err);
    zbra_index_free(&idx);
}

/* --------------------------------------------------------------- json */

static const char *JSON_SAMPLE =
    "{\n"
    "  \"format\": 1,\n"
    "  \"packages\": [\n"
    "    { \"name\": \"native-a\", \"version\": \"2.0\", \"format\": \"tar.xz\",\n"
    "      \"filename\": \"native-a-2.0.tar.xz\", \"size\": 2048,\n"
    "      \"depends\": [\"libfoo\", \"libbar >= 1.2\"],\n"
    "      \"description\": \"a native tar.xz package\" },\n"
    "    { \"name\": \"native-b\", \"version\": \"0.9.1\", \"format\": \"tar.xz\",\n"
    "      \"filename\": \"native-b-0.9.1.tar.xz\" }\n"
    "  ]\n"
    "}\n";

static void test_json(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;
    zbra_package p;

    puts("index: zbra index.json parsing");

    mktemp_dir("json");
    path = write_tmp("index.json", JSON_SAMPLE);

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "local", "file:///srv", &err) == 0,
       "index.json parses");
    ok(idx.n == 2, "two records");
    ok(zbra_index_find(&idx, "native-a", &p) == 1, "native-a found");
    ok(p.n_depends == 2, "depends array parsed");
    ok(strcmp(p.depends[1], "libbar >= 1.2") == 0,
       "constraint string preserved verbatim");
    ok(p.size == 2048, "numeric field parsed");
    ok(strcmp(p.format, "tar.xz") == 0, "format parsed");
    ok(zbra_index_find(&idx, "native-b", &p) == 1 && p.n_depends == 0,
       "absent depends array leaves an empty list");

    zbra_index_free(&idx);
    free(err);
}

static void test_json_bare_array(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: a bare top-level array is accepted");

    mktemp_dir("jsonarray");
    path = write_tmp("index.json",
                     "[{\"name\":\"x\",\"version\":\"1\",\"format\":\"tar.xz\"}]");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == 0, "parses");
    ok(idx.n == 1, "one record");
    zbra_index_free(&idx);
    free(err);
}

static void test_json_rejects_garbage(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: a non-index file is rejected, not half-read");

    mktemp_dir("jsonbad");
    path = write_tmp("index.json", "<html>404 not found</html>");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == -1, "rejected");
    ok(err != NULL, "with an explanation");
    free(err);
    zbra_index_free(&idx);
}

static void test_json_escapes(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;
    zbra_package p;

    puts("index: escapes and unicode decode correctly");

    mktemp_dir("jsonesc");
    path = write_tmp("index.json",
                     "[{\"name\":\"q\",\"version\":\"1\",\"format\":\"tar.xz\","
                     "\"description\":\"tab\\there \\\"quoted\\\" \\\\slash\\n"
                     "newline \\u0041\"}]");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == 0, "parses");
    ok(zbra_index_find(&idx, "q", &p) == 1, "record found");
    if (p.description != NULL) {
        ok(strchr(p.description, '\t') != NULL, "\\t decoded");
        ok(strstr(p.description, "\"quoted\"") != NULL, "\\\" decoded");
        ok(strstr(p.description, "\\slash") != NULL, "\\\\ decoded");
        ok(strstr(p.description, "\n") != NULL, "\\n decoded");
        ok(strstr(p.description, "A") != NULL, "\\u0041 decoded");
    } else {
        ok(0, "description was parsed");
    }

    zbra_index_free(&idx);
    free(err);
}

static void test_json_ignores_unknown_keys(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: unknown fields are skipped, not fatal");

    mktemp_dir("jsonunk");
    path = write_tmp("index.json",
                     "[{\"name\":\"y\",\"version\":\"1\",\"format\":\"tar.xz\","
                     "\"future_field\":{\"a\":[1,2,{\"b\":\"c\"}]},"
                     "\"n\":12345}]");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == 0,
       "unknown fields skipped");
    ok(idx.n == 1, "record still indexed");
    zbra_index_free(&idx);
    free(err);
}

static void test_json_roundtrip(void)
{
    zbra_index a, b;
    char      *err = NULL;
    char       path[1024];
    zbra_package p;

    puts("index: write then read preserves every field");

    mktemp_dir("rt");

    zbra_index_init(&a);
    {
        zbra_package q;

        memset(&q, 0, sizeof(q));
        q.name = strdup("round-trip");
        q.version = strdup("1:2.3-4");
        q.format = strdup("deb");
        q.source_id = strdup("tenebra");
        q.filename = strdup("pool/rt.deb");
        q.checksum = strdup("sha256:deadbeef");
        q.description = strdup("quotes \" and \\ and newline\nhere");
        q.homepage = strdup("https://tenebra.org");
        q.license = strdup("MIT");
        q.size = 4096;
        q.style = ZBRA_VER_DEB;

        {
            const char *d[2];
            d[0] = "libfoo";
            d[1] = "libbar (>= 2)";
            zbra_d822_strlist_free(q.depends, q.n_depends);
            q.depends = calloc(2, sizeof(char *));
            q.depends[0] = strdup(d[0]);
            q.depends[1] = strdup(d[1]);
            q.n_depends = 2;
        }

        {
            zbra_package *grown = realloc(a.items,
                                          (a.n + 1) * sizeof(*grown));
            if (grown == NULL)
                exit(1);
            a.items = grown;
            a.items[a.n++] = q;
        }
    }

    snprintf(path, sizeof(path), "%s/index.json", g_dir);
    ok(zbra_index_write_json(&a, path, &err) == 0, "write succeeds");

    zbra_index_init(&b);
    ok(zbra_index_read_json(&b, path, "s", "u", &err) == 0, "read back");
    ok(b.n == 1, "one record");

    if (zbra_index_find(&b, "round-trip", &p) == 1) {
        ok(strcmp(p.version, "1:2.3-4") == 0, "version survived, epoch and all");
        ok(strcmp(p.checksum, "sha256:deadbeef") == 0, "checksum survived");
        ok(p.n_depends == 2, "depends survived");
        if (p.description == NULL) {
            ok(0, "awkward description survived intact (got NULL)");
        } else {
            ok(strstr(p.description, "quotes \"") != NULL,
               "escaped quotes survived");
            ok(strstr(p.description, "and \\ and") != NULL,
               "escaped backslash survived");
            ok(strstr(p.description, "\n") != NULL, "newline survived");
        }
        ok(p.size == 4096, "size survived");
        ok(strcmp(p.license, "MIT") == 0, "license survived");
    } else {
        ok(0, "record survived the round-trip");
    }

    zbra_index_free(&a);
    zbra_index_free(&b);
    free(err);
}

static void test_write_json_is_atomic(void)
{
    zbra_index idx;
    char      *err = NULL;
    char       path[1024];
    char       tmp[1024];

    puts("index: writing an index cannot leave a partial file");

    mktemp_dir("atomic");
    zbra_index_init(&idx);

    snprintf(path, sizeof(path), "%s/index.json", g_dir);
    snprintf(tmp, sizeof(tmp), "%s/index.json.tmp", g_dir);

    ok(zbra_index_write_json(&idx, path, &err) == 0, "write succeeds");

    {
        struct stat st;
        ok(stat(tmp, &st) != 0, "temporary file cleaned up");
        ok(stat(path, &st) == 0, "final file present");
    }

    /* An empty index is still valid JSON, not an empty file. */
    {
        FILE *fp = fopen(path, "r");
        char  buf[256];
        size_t n;

        ok(fp != NULL, "file readable");
        n = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[n] = '\0';
        if (fp != NULL)
            fclose(fp);
        ok(strstr(buf, "\"packages\"") != NULL,
           "empty index is still well-formed");
    }

    zbra_index_free(&idx);
    free(err);
}

/* --------------------------------------------------------------- queries */

static void test_find_picks_newest(void)
{
    zbra_index idx;
    zbra_package p;
    char      *err = NULL;
    char      *path;

    puts("index: find returns the newest version of a name");

    mktemp_dir("newest");
    path = write_tmp("Packages",
                     "Package: multi\nVersion: 1.9\nFilename: a.deb\n"
                     "\n"
                     "Package: multi\nVersion: 1.10\nFilename: b.deb\n"
                     "\n"
                     "Package: multi\nVersion: 1.10~rc1\nFilename: c.deb\n"
                     "\n");

    zbra_index_init(&idx);
    ok(zbra_index_read_deb_packages(&idx, path, "s", "u", &err) == 0,
       "parses three versions");
    ok(zbra_index_find(&idx, "multi", &p) == 1, "found");

    /*
     * 1.10 > 1.9 numerically, and 1.10 > 1.10~rc1 because '~' sorts before
     * everything. Getting either wrong is how a package manager installs a
     * release candidate over a stable release.
     */
    ok(strcmp(p.version, "1.10") == 0, "1.10 wins over 1.9 and over 1.10~rc1");

    ok(zbra_index_find(&idx, "absent", &p) == 0, "absent name reports not found");

    zbra_index_free(&idx);
    free(err);
}

static void test_search(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;
    zbra_package *hits[4];
    size_t     n;

    puts("index: search matches names and descriptions");

    mktemp_dir("search");
    path = write_tmp("Packages",
                     "Package: vim\nVersion: 9.1\n"
                     "Description: Vi IMproved\n\n"
                     "Package: neovim\nVersion: 0.10\n"
                     "Description: fork of vim\n\n"
                     "Package: emacs\nVersion: 29\n"
                     "Description: the extensible editor\n\n");

    zbra_index_init(&idx);
    zbra_index_read_deb_packages(&idx, path, "s", "u", &err);

    n = zbra_index_search(&idx, "vim", hits, 4);
    ok(n == 2, "two packages mention vim");
    if (n == 2) {
        ok(strcmp(hits[0]->name, "vim") == 0,
           "exact name match sorts first");
    }

    n = zbra_index_search(&idx, "extensible", hits, 4);
    ok(n == 1, "description-only match found");

    n = zbra_index_search(&idx, "VIM", hits, 4);
    ok(n == 2, "search is case-insensitive");

    n = zbra_index_search(&idx, "nothingmatchesthis", hits, 4);
    ok(n == 0, "no matches returns zero");

    /* A short buffer must be respected, never written past. */
    n = zbra_index_search(&idx, "vim", hits, 1);
    ok(n == 1, "a short buffer is respected");
    ok(hits[0] != NULL && strcmp(hits[0]->name, "vim") == 0,
       "the closest match fits in one slot");

    n = zbra_index_search(&idx, "vim", NULL, 0);
    ok(n == 2, "counting without an output array works");

    n = zbra_index_search(&idx, "vim", hits, 0);
    ok(n == 0, "zero capacity stores nothing");

    zbra_index_free(&idx);
    free(err);
}

static void test_merge(void)
{
    zbra_index a, b;
    char      *err = NULL;
    char      *p1, *p2;
    zbra_package p;

    puts("index: merge keeps the higher version on conflict");

    mktemp_dir("merge");
    p1 = write_tmp("one", "Package: shared\nVersion: 1.0\nFilename: a\n\n"
                          "Package: only-in-a\nVersion: 1\nFilename: b\n\n");
    p2 = write_tmp("two", "Package: shared\nVersion: 2.0\nFilename: c\n\n"
                          "Package: only-in-b\nVersion: 1\nFilename: d\n\n");

    zbra_index_init(&a);
    zbra_index_init(&b);
    zbra_index_read_deb_packages(&a, p1, "one", "u1", &err);
    zbra_index_read_deb_packages(&b, p2, "two", "u2", &err);

    ok(a.n == 2 && b.n == 2, "two records each");
    ok(zbra_index_merge(&a, &b) == 0, "merge succeeds");
    ok(a.n == 3, "shared collapsed, two uniques kept");

    ok(zbra_index_find(&a, "shared", &p) == 1 &&
       strcmp(p.version, "2.0") == 0, "the higher version won");
    ok(zbra_index_find(&a, "only-in-a", &p) == 1, "a-only record kept");
    ok(zbra_index_find(&a, "only-in-b", &p) == 1, "b-only record merged in");

    zbra_index_free(&a);
    zbra_index_free(&b);
    free(err);
}

static void test_sort_is_stable(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: sorting produces a deterministic listing");

    mktemp_dir("sort");
    path = write_tmp("Packages",
                     "Package: zeta\nVersion: 1\n\n"
                     "Package: alpha\nVersion: 1\n\n"
                     "Package: mid\nVersion: 1\n\n");

    zbra_index_init(&idx);
    zbra_index_read_deb_packages(&idx, path, "s", "u", &err);
    zbra_index_sort(&idx);

    ok(idx.n == 3, "three records");
    ok(strcmp(idx.items[0].name, "alpha") == 0, "sorted first");
    ok(strcmp(idx.items[2].name, "zeta") == 0, "sorted last");

    zbra_index_free(&idx);
    free(err);
}

/* -------------------------------------------------------- hostile input */

static void test_hostile_index(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: hostile records are dropped, not trusted");

    mktemp_dir("hostile");
    path = write_tmp("Packages",
                     /* Traversal in the package name. */
                     "Package: ../../etc/shadow\n"
                     "Version: 1\n"
                     "Filename: evil.deb\n"
                     "\n"
                     /* Absolute path as a package name. */
                     "Package: /etc/passwd\n"
                     "Version: 1\n"
                     "Filename: evil.deb\n"
                     "\n"
                     /* A shell metacharacter in the name. */
                     "Package: foo;rm -rf /\n"
                     "Version: 1\n"
                     "Filename: evil.deb\n"
                     "\n"
                     /* Missing version. */
                     "Package: noversion\n"
                     "Filename: evil.deb\n"
                     "\n"
                     /* A perfectly good record, to prove the file is not */
                     /* rejected wholesale because of the others. */
                     "Package: honest\n"
                     "Version: 1.0\n"
                     "Filename: honest.deb\n"
                     "\n");

    zbra_index_init(&idx);
    ok(zbra_index_read_deb_packages(&idx, path, "evil", "u", &err) == 0,
       "the file still parses");

    /*
     * Exactly one record survives. If a validation change ever rejected the
     * whole file, or accepted a hostile name, these assertions fail.
     */
    ok(idx.n == 1, "only the honest record survived");
    ok(idx.n == 1 && strcmp(idx.items[0].name, "honest") == 0,
       "and it is the right one");
    ok(idx.n_warnings == 4, "four dropped records were reported");

    /*
     * Every warning must state a reason. A generic "skipped a record" tells
     * the user nothing about whether their repository is broken or hostile.
     */
    {
        size_t i;
        int    all_named = 1;

        for (i = 0; i < idx.n_warnings; i++) {
            if (idx.warnings[i] == NULL ||
                (strstr(idx.warnings[i], "unsafe") == NULL &&
                 strstr(idx.warnings[i], "missing") == NULL))
                all_named = 0;
        }
        ok(all_named, "every warning states a reason");
        ok(idx.n_warnings == 4, "one warning per dropped record");
    }

    /* The hostile names must not be reachable through lookup. */
    {
        zbra_package p;
        ok(zbra_index_find(&idx, "../../etc/shadow", &p) == 0,
           "traversal name is not findable");
        ok(zbra_index_find(&idx, "/etc/passwd", &p) == 0,
           "absolute name is not findable");
    }

    zbra_index_free(&idx);
    free(err);
}

static void test_hostile_json(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: hostile index.json records are dropped too");

    mktemp_dir("hostilejson");
    path = write_tmp("index.json",
                     "[{\"name\":\"../escape\",\"version\":\"1\","
                     "\"format\":\"tar.xz\"},"
                     "{\"name\":\"ok\",\"version\":\"1\","
                     "\"format\":\"tar.xz\"}]");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == 0, "parses");
    ok(idx.n == 1, "traversal name dropped");
    ok(idx.n == 1 && strcmp(idx.items[0].name, "ok") == 0, "valid kept");

    zbra_index_free(&idx);
    free(err);
}

static void test_truncated_json_is_survivable(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: a truncated download does not crash the parser");

    mktemp_dir("trunc");
    path = write_tmp("index.json",
                     "[{\"name\":\"a\",\"version\":\"1\",\"format\":\"tar.xz\"},"
                     "{\"name\":\"b\",\"versi");

    zbra_index_init(&idx);
    ok(zbra_index_read_json(&idx, path, "s", "u", &err) == 0,
       "truncation is survivable rather than fatal");
    ok(idx.n >= 1, "at least the complete record was recovered");

    zbra_index_free(&idx);
    free(err);
}

/*
 * The resolver bridge.
 *
 * deps.c must not know how an index is stored, so it asks for candidates
 * through callbacks. This checks the two halves actually fit: the resolver
 * walks an index's dependency graph and produces an ordered plan.
 */
/*
 * read_auto has to tell a JSON index from a Debian Packages file by suffix.
 *
 * This is the only thing standing between a repository's index.json and the
 * Debian stanza parser, and getting it wrong is silent: the parse "succeeds",
 * finds no stanzas, and reports an empty repository. So each suffix is checked
 * explicitly, including the short names where an off-by-one in the suffix
 * length would hide.
 */
static void test_read_auto_picks_the_parser(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;

    puts("index: read_auto chooses a parser from the file name");

    path = write_tmp("auto.json",
                     "{\"format\":1,\"packages\":[{\"name\":\"autojson\","
                     "\"version\":\"1.0\",\"format\":\"tar.xz\"}]}\n");
    zbra_index_init(&idx);
    ok(zbra_index_read_auto(&idx, path, "s", "file:///x", &err) == 0,
       "a .json file is read");
    ok(idx.n == 1 && idx.items[0].name != NULL &&
       strcmp(idx.items[0].name, "autojson") == 0,
       "a .json file is parsed as JSON, not as Packages");
    zbra_index_free(&idx);

    /* The shortest name that still carries the suffix. */
    path = write_tmp("a.json", "{\"format\":1,\"packages\":[]}\n");
    zbra_index_init(&idx);
    ok(zbra_index_read_auto(&idx, path, "s", "file:///x", &err) == 0,
       "a five character .json name is read");
    zbra_index_free(&idx);

    /* A .deb file is not valid JSON but is not rejected as one either. */
    path = write_tmp("auto.deb", "Package: autodeb\nVersion: 2.0\n\n");
    zbra_index_init(&idx);
    ok(zbra_index_read_auto(&idx, path, "s", "file:///x", &err) == 0,
       "a .deb file is read");
    ok(idx.n == 1 && idx.items[0].name != NULL &&
       strcmp(idx.items[0].name, "autodeb") == 0,
       "a .deb file is parsed as Packages");
    zbra_index_free(&idx);

    /* No suffix: the Debian parser is the fallback. */
    path = write_tmp("Packages", "Package: plain\nVersion: 3.0\n\n");
    zbra_index_init(&idx);
    ok(zbra_index_read_auto(&idx, path, "s", "file:///x", &err) == 0,
       "a bare Packages file is read");
    ok(idx.n == 1 && idx.items[0].name != NULL &&
       strcmp(idx.items[0].name, "plain") == 0,
       "a bare Packages file is parsed as Packages");
    zbra_index_free(&idx);

    /* An absent file is an error with a reason, not a crash. */
    zbra_index_init(&idx);
    err = NULL;
    ok(zbra_index_read_auto(&idx, "/nonexistent/zbra/index.json", "s", "u",
                            &err) != 0,
       "a missing file is reported");
    ok(err != NULL, "a missing file has a reason");
    free(err);
    zbra_index_free(&idx);

    /* A file name that merely contains ".json" is not treated as JSON. */
    path = write_tmp("index.json.txt", "Package: sneaky\nVersion: 1.0\n\n");
    zbra_index_init(&idx);
    ok(zbra_index_read_auto(&idx, path, "s", "file:///x", &err) == 0,
       "a .json.txt file is read");
    ok(idx.n == 1 && idx.items[0].name != NULL &&
       strcmp(idx.items[0].name, "sneaky") == 0,
       "a .json.txt file is parsed as Packages, not as JSON");
    zbra_index_free(&idx);
}

/*
 * A scanned record has to carry a digest.
 *
 * A file name cannot state one, so the scanner computes it. Without it every
 * record from a directory source would be unverifiable, and since install
 * refuses what it cannot verify, the whole directory-source feature would be
 * unusable rather than merely cautious.
 */
static void test_scan_records_a_checksum(void)
{
    zbra_index idx;
    char      *err = NULL;
    char      *path;
    size_t     i;
    int        found = 0;

    puts("index: a scanned directory records digests");

    {
        static const char body[] = "not really an archive\n";

        path = write_tmp("scanned-2.1.tar.xz", body);
    }

    zbra_index_init(&idx);
    ok(zbra_index_scan_directory(&idx, g_dir, "s", "file:///x", &err) == 0,
       "the directory scans");

    for (i = 0; i < idx.n; i++) {
        if (strcmp(idx.items[i].name, "scanned") != 0)
            continue;

        found = 1;
        ok(idx.items[i].checksum != NULL,
           "a scanned record has a checksum");
        ok(idx.items[i].checksum != NULL &&
           strncmp(idx.items[i].checksum, "sha256:", 7) == 0,
           "the checksum names its algorithm");

        /* And it has to be the digest of that file, not of something else. */
        {
            char          hex[80];
            unsigned char digest[32];
            char          want[96];

            ok(zbra_sha256_file(path, hex) == 0, "the fixture digests");

            {
                static const char body[] = "not really an archive\n";

                zbra_sha256_buf(body, sizeof(body) - 1, digest);
            }
            zbra_sha256_hex(digest, want + 7);
            memcpy(want, "sha256:", 7);

            ok(idx.items[i].checksum != NULL &&
               strcmp(idx.items[i].checksum, want) == 0,
               "the checksum is the digest of the file that was scanned");
            if (idx.items[i].checksum != NULL &&
                strcmp(idx.items[i].checksum, want) != 0)
                printf("    expected %s\n    got      %s\n", want,
                       idx.items[i].checksum);
        }
    }

    ok(found, "the scanned package is in the index");
    zbra_index_free(&idx);
}

static void test_candidate_bridge(void)
{
    zbra_index  idx;
    zbra_plan   plan;
    char       *err = NULL;
    const char *path;
    char       *installed = NULL;

    zbra_resolver r;

    path = write_tmp("bridge.Packages",
        "Package: app\nVersion: 2.0\nDepends: libc\n\n"
        "Package: libc\nVersion: 1.0\n\n");

    zbra_index_init(&idx);
    ok(zbra_index_read_deb_packages(&idx, path, "s", "u", &err) == 0,
       "bridge index parses");

    r.find_candidate    = zbra_index_find_candidate;
    r.installed_version = NULL;
    r.ud                = &idx;
    r.ud_installed      = NULL;

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == 0,
       "resolver accepts an index as its candidate source");
    ok(plan.n == 2, "both app and libc landed in the plan");
    ok(plan.n == 2 && plan.items[0].name != NULL &&
       strcmp(plan.items[0].name, "libc") == 0,
       "the dependency is installed first");
    ok(plan.n == 2 && strcmp(plan.items[1].name, "app") == 0,
       "the requested package comes last");

    zbra_plan_free(&plan);

    /* A package nobody offers must fail, not silently do nothing. */
    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "ghost", &plan, &err) == -1,
       "an unknown target is an error");
    ok(err != NULL, "the failure explains itself");
    free(err);
    err = NULL;
    zbra_plan_free(&plan);

    zbra_index_free(&idx);
    (void)installed;
}

int main(void)
{
    /*
     * Line-buffered so that a crash mid-test still leaves the passing
     * assertions on screen. Fully buffered stdout is discarded when the
     * process dies, which hides the very failure being hunted.
     */
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_deb_packages();
    test_deb_missing_file();

    test_json();
    test_json_bare_array();
    test_json_rejects_garbage();
    test_json_escapes();
    test_json_ignores_unknown_keys();
    test_json_roundtrip();
    test_write_json_is_atomic();

    test_find_picks_newest();
    test_search();
    test_merge();
    test_sort_is_stable();

    test_hostile_index();
    test_hostile_json();
    test_truncated_json_is_survivable();

    test_read_auto_picks_the_parser();

    test_scan_records_a_checksum();

    test_candidate_bridge();

    printf("\nindex: %d passed, %d failed\n", tests_run, tests_failed);

    return tests_failed != 0;
}
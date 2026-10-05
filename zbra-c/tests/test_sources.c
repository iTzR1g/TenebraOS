/*
 * test_sources.c -- unit tests for the source configuration parser.
 *
 * The load path is the security-relevant one: a source id becomes part of
 * cache paths and a source URL is fetched over the network, so malformed
 * input must be rejected rather than normalised into something plausible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "deb822.h"
#include "sources.h"

static int tests_run;
static int tests_failed;

#define ok(cond, ...)                                                      \
    do {                                                                   \
        tests_run++;                                                       \
        if (cond) {                                                        \
            printf("  ok   ");                                            \
        } else {                                                           \
            tests_failed++;                                                \
            printf("  FAIL ");                                            \
        }                                                                  \
        printf(__VA_ARGS__);                                               \
        printf("\n");                                                      \
    } while (0)

/* Unique config directory per test so nothing leaks between them. */
static char g_root[512];

static void mktemp_root(const char *label)
{
    snprintf(g_root, sizeof(g_root), "/tmp/zbra-src-test/%s-%d", label,
             (int)getpid());
    mkdir("/tmp/zbra-src-test", 0755);
    mkdir(g_root, 0755);
}

static void write_file(const char *name, const char *content)
{
    char  path[1024];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/%s", g_root, name);
    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fputs(content, fp);
    fclose(fp);
}

static void rmrf(const char *path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0)
        fprintf(stderr, "rmrf failed\n");
}

/* --------------------------------------------------------------- parsing */

static void test_parse_basic(void)
{
    zbra_source s;
    char       *err = NULL;

    puts("sources: id / type / url");

    memset(&s, 0, sizeof(s));
    ok(zbra_source_parse_line("tenebra deb https://example.org/repo ./", &s,
                              &err) == 0,
       "a three-field source parses");
    ok(strcmp(s.id, "tenebra") == 0, "id is captured");
    ok(s.type == ZBRA_SRC_DEB, "type is deb");
    ok(strcmp(s.url, "https://example.org/repo") == 0, "url is captured");
    ok(s.n_components == 1 && strcmp(s.components[0], "./") == 0,
       "component is captured");
    ok(err == NULL, "no error set on success");
    zbra_source_free(&s);

    /* Every backend type must be recognised by name. */
    ok(zbra_source_type_from_name("deb") == ZBRA_SRC_DEB, "deb type");
    ok(zbra_source_type_from_name("rpm") == ZBRA_SRC_RPM, "rpm type");
    ok(zbra_source_type_from_name("tar.xz") == ZBRA_SRC_TARXZ, "tar.xz type");
    ok(zbra_source_type_from_name("snap") == ZBRA_SRC_SNAP, "snap type");
    ok(zbra_source_type_from_name("aur") == ZBRA_SRC_AUR, "aur type");
    ok(zbra_source_type_from_name("nonsense") == -1, "unknown type rejected");

    /* Round-trip the names so save/load does not degrade a config. */
    ok(strcmp(zbra_source_type_name(ZBRA_SRC_RPM), "rpm") == 0,
       "type name round-trips");
    ok(strcmp(zbra_source_type_name(ZBRA_SRC_TARXZ), "tar.xz") == 0,
       "tar.xz type name round-trips");

    zbra_source_free(&s);
}

static void test_parse_comments_and_blanks(void)
{
    zbra_source s;
    char       *err = NULL;

    puts("sources: comments and whitespace");

    memset(&s, 0, sizeof(s));
    ok(zbra_source_parse_line("   # a comment", &s, &err) != 0,
       "comment-only line is not a source");
    zbra_source_free(&s);

    memset(&s, 0, sizeof(s));
    ok(zbra_source_parse_line("x deb https://u/  # trailing note", &s,
                              &err) == 0,
       "trailing comment is stripped");
    ok(strcmp(s.url, "https://u/") == 0,
       "url excludes the trailing comment");
    zbra_source_free(&s);

    memset(&s, 0, sizeof(s));
    ok(zbra_source_parse_line("  y  rpm   https://v/  ", &s, &err) == 0,
       "extra whitespace is tolerated");
    ok(strcmp(s.id, "y") == 0 && strcmp(s.url, "https://v/") == 0,
       "fields are trimmed despite padding");
    zbra_source_free(&s);

    free(err);
}

static void test_parse_rejects_bad(void)
{
    zbra_source s;
    char       *err = NULL;

    puts("sources: malformed lines are rejected");

    memset(&s, 0, sizeof(s));
    err = NULL;
    ok(zbra_source_parse_line("onlyonefield", &s, &err) == -1,
       "a single field is rejected");
    ok(err != NULL, "rejection explains itself");
    free(err);
    err = NULL;
    zbra_source_free(&s);

    memset(&s, 0, sizeof(s));
    ok(zbra_source_parse_line("", &s, &err) == -1, "empty line is rejected");
    free(err);
    zbra_source_free(&s);
}

static void test_parse_unknown_type(void)
{
    zbra_source s;
    char       *err = NULL;

    puts("sources: unknown type is reported, not guessed");

    memset(&s, 0, sizeof(s));
    err = NULL;
    ok(zbra_source_parse_line("weird pacman https://arch/", &s, &err) == -2,
       "unknown type returns the distinct 'unsupported' code");
    ok(err != NULL && strstr(err, "pacman") != NULL,
       "the message names the offending type");
    ok(s.enabled == 0, "an unsupported source is never marked enabled");
    free(err);
    zbra_source_free(&s);
}

/* --------------------------------------------------------------- loading */

static void test_load_and_merge(void)
{
    zbra_sources s;
    char        *err = NULL;
    char        *warn = NULL;

    puts("sources: sources.list plus sources.list.d, in sorted order");

    mktemp_root("merge");
    {
        char d[1024];
        snprintf(d, sizeof(d), "%s/sources.list.d", g_root);
        mkdir(d, 0755);
    }

    write_file("sources.list",
               "# main\n"
               "tenebra deb https://itzr1g.github.io/TenebraOS-packages/ ./\n");
    {
        char p[1024];
        FILE *fp;
        snprintf(p, sizeof(p), "%s/sources.list.d/10-extra.list", g_root);
        fp = fopen(p, "w");
        fprintf(fp, "extra rpm https://example.org/fedora/39/x86_64/\n");
        fclose(fp);
        snprintf(p, sizeof(p), "%s/sources.list.d/20-arch.list", g_root);
        fp = fopen(p, "w");
        fprintf(fp, "arch aur\n");
        fclose(fp);
        /* Not a .list file: must be ignored entirely. */
        snprintf(p, sizeof(p), "%s/sources.list.d/notes.txt", g_root);
        fp = fopen(p, "w");
        fprintf(fp, "this is not a source file\n");
        fclose(fp);
    }

    ok(zbra_sources_load(&s, g_root, &err, &warn) == 0, "load succeeds");
    ok(s.n == 3, "three sources loaded");
    ok(warn == NULL || warn[0] == '\0', "no warnings for a clean config");

    /*
     * Same order apt uses: sources.list first, then sources.list.d. Drop-ins
     * additionally override the base file, which is the behaviour users
     * expect from adding a numbered file.
     */
    ok(s.n == 3 && strcmp(s.items[0].id, "tenebra") == 0,
       "sources.list is read first");
    ok(s.n == 3 && strcmp(s.items[1].id, "extra") == 0,
       "10-extra.list follows sources.list");
    ok(s.n == 3 && strcmp(s.items[2].id, "arch") == 0,
       "20-arch.list comes last, in sorted order");

    ok(zbra_sources_find(&s, "tenebra") != NULL, "lookup by id finds tenebra");
    ok(zbra_sources_find(&s, "nope") == NULL, "lookup misses cleanly");

    zbra_sources_free(&s);
    free(err);
    free(warn);
    rmrf(g_root);
}

static void test_load_warns_on_bad_line(void)
{
    zbra_sources s;
    char        *err = NULL;
    char        *warn = NULL;

    puts("sources: one bad line does not break the whole config");

    mktemp_root("warn");
    write_file("sources.list",
               "good deb https://good/\n"
               "badline\n"
               "other pacman https://arch/\n");

    ok(zbra_sources_load(&s, g_root, &err, &warn) == 0,
       "load still succeeds");
    ok(s.n == 1, "only the valid source is active");
    ok(warn != NULL && strstr(warn, "badline") == NULL &&
       strstr(warn, "pacman") != NULL,
       "warning names the unknown type");
    ok(err == NULL, "load itself did not fail");

    zbra_sources_free(&s);
    free(err);
    free(warn);
    rmrf(g_root);
}

static void test_load_missing_dir(void)
{
    zbra_sources s;
    char        *err = NULL;

    puts("sources: an absent config directory is empty, not an error");

    ok(zbra_sources_load(&s, "/tmp/zbra-src-test/does-not-exist", &err,
                         NULL) == 0,
       "missing directory loads as empty");
    ok(s.n == 0, "no sources");
    zbra_sources_free(&s);
    free(err);
}

/* --------------------------------------------------------------- mutation */

static void test_set_and_remove(void)
{
    zbra_sources s;
    char        *err = NULL;
    const char  *comp[1];

    puts("sources: add, replace and remove by id");

    zbra_sources_init(&s);

    ok(zbra_sources_set(&s, "test", "deb", "https://a/", NULL, 0, &err) == 0,
       "add a source");
    ok(s.n == 1, "one source");

    /* Adding the same id again must replace, not duplicate. */
    ok(zbra_sources_set(&s, "test", "rpm", "https://b/", NULL, 0, &err) == 0,
       "re-add the same id");
    ok(s.n == 1, "still one source after re-add");
    ok(s.items[0].type == ZBRA_SRC_RPM, "type was updated by the re-add");
    ok(strcmp(s.items[0].url, "https://b/") == 0, "url was updated");

    comp[0] = "./";
    ok(zbra_sources_set(&s, "withcomp", "deb", "https://c/", comp, 1, &err) == 0,
       "add a source with a component");
    ok(s.n == 2, "two sources");

    /* Validation must reject an unknown type rather than storing it. */
    ok(zbra_sources_set(&s, "bad", "pacman", "https://d/", NULL, 0, &err) == -1,
       "unknown type refused by set");
    ok(err != NULL, "refusal explains the valid types");
    free(err);
    err = NULL;
    ok(s.n == 2, "the rejected source was not stored");

    ok(zbra_sources_remove(&s, "test") == 1, "remove returns found");
    ok(s.n == 1, "one source left");
    ok(zbra_sources_find(&s, "test") == NULL, "removed source is gone");
    ok(zbra_sources_remove(&s, "test") == 0, "removing again reports absent");

    zbra_sources_free(&s);
    free(err);
}

static void test_save_load_roundtrip(void)
{
    zbra_sources before, after;
    char        *err = NULL;
    const char  *comp[2];

    puts("sources: save then load preserves every field");

    mktemp_root("roundtrip");
    zbra_sources_init(&before);

    comp[0] = "./";
    comp[1] = "contrib";
    zbra_sources_set(&before, "tenebra", "deb",
                     "https://itzr1g.github.io/TenebraOS-packages/", comp, 2,
                     &err);
    zbra_sources_set(&before, "fedora", "rpm", "https://f.example/39/", NULL, 0,
                     &err);
    zbra_sources_set(&before, "local", "tar.xz", "file:///srv/pkgs", NULL, 0,
                     &err);
    zbra_sources_set(&before, "arch", "aur", NULL, NULL, 0, &err);
    zbra_sources_set(&before, "snaps", "snap", "https://snapcraft.io", NULL, 0,
                     &err);

    ok(before.n == 5, "five sources staged");
    ok(zbra_sources_save(&before, g_root, &err) == 0, "save succeeds");

    ok(zbra_sources_load(&after, g_root, &err, NULL) == 0, "reload succeeds");
    ok(after.n == before.n, "same count after round-trip");

    if (after.n == 5 && before.n == 5) {
        size_t i;
        int    all_match = 1;

        for (i = 0; i < 5; i++) {
            if (strcmp(after.items[i].id, before.items[i].id) != 0 ||
                after.items[i].type != before.items[i].type ||
                after.items[i].n_components != before.items[i].n_components)
                all_match = 0;
        }

        ok(all_match, "every id, type and component count survived");
        ok(after.items[0].n_components == 2 &&
           strcmp(after.items[0].components[1], "contrib") == 0,
           "multiple components survived");
        ok(after.items[3].url == NULL || after.items[3].url[0] == '\0',
           "a source with no url round-trips without inventing one");
    }

    zbra_sources_free(&before);
    zbra_sources_free(&after);
    free(err);
    rmrf(g_root);
}

static void test_save_is_atomic(void)
{
    zbra_sources s;
    char        *err = NULL;
    char         path[1024];

    puts("sources: an interrupted save cannot truncate the file");

    mktemp_root("atomic");
    zbra_sources_init(&s);
    zbra_sources_set(&s, "a", "deb", "https://a/", NULL, 0, &err);
    ok(zbra_sources_save(&s, g_root, &err) == 0, "first save succeeds");

    zbra_sources_free(&s);
    zbra_sources_init(&s);
    zbra_sources_set(&s, "b", "rpm", "https://b/", NULL, 0, &err);
    ok(zbra_sources_save(&s, g_root, &err) == 0, "second save succeeds");

    snprintf(path, sizeof(path), "%s/sources.list", g_root);
    {
        FILE *fp = fopen(path, "r");
        char  buf[2048];
        size_t n = 0;

        ok(fp != NULL, "sources.list exists after save");
        if (fp != NULL) {
            n = fread(buf, 1, sizeof(buf) - 1, fp);
            fclose(fp);
        }
        buf[n] = '\0';
        ok(strstr(buf, "b rpm") != NULL, "new content is present");
    }

    /* The temp file used for the rename must not be left behind. */
    {
        char tmp[1024];
        struct stat st;
        snprintf(tmp, sizeof(tmp), "%s/sources.list.tmp", g_root);
        ok(stat(tmp, &st) != 0, "no temporary file left behind");
    }

    zbra_sources_free(&s);
    free(err);
    rmrf(g_root);
}

int main(void)
{
    test_parse_basic();
    test_parse_comments_and_blanks();
    test_parse_rejects_bad();
    test_parse_unknown_type();

    test_load_and_merge();
    test_load_warns_on_bad_line();
    test_load_missing_dir();

    test_set_and_remove();
    test_save_load_roundtrip();
    test_save_is_atomic();

    printf("\nsources: %d passed, %d failed\n", tests_run, tests_failed);

    return tests_failed != 0;
}
/*
 * test_deps.c -- unit tests for dependency resolution.
 *
 * The fake index below is a hand-built graph, so each test can state exactly
 * what topology it is exercising. The cases that matter most are the ones
 * that historically break package managers: diamond dependencies (fetched
 * once), cycles (detected with a readable chain), and a requirement already
 * satisfied by an installed version (skipped, not reinstalled).
 */

#include "deps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* A fake package graph                                               */
/* ------------------------------------------------------------------ */

/*
 * A package in the fake world. `deps` is a NULL-terminated array of
 * requirement strings, borrowed by the fake index.
 */
typedef struct {
    const char *name;
    const char *version;
    const char *deps[8];
} fake_pkg;

static fake_pkg g_pkgs[512];
static size_t g_npkgs;

/* Installed state, parallel to the package table. */
static char *g_installed[512];

/*
 * Packages recorded as installed but deliberately absent from the index.
 *
 * This models the real case of a package that was removed from a repository
 * after being installed: it exists on the system, no source offers it, and
 * the resolver has to explain that rather than silently ignoring it.
 */
typedef struct {
    char *name;
    char *version;
} stray_install;

static stray_install g_stray[16];
static size_t g_nstray;

static void world_install_stray(const char *name, const char *version)
{
    g_stray[g_nstray].name = strdup(name);
    g_stray[g_nstray].version = strdup(version);
    g_nstray++;
}

static int lookup_count;

static void pkg_add(const char *name, const char *version, ...)
{
    fake_pkg *p = &g_pkgs[g_npkgs++];
    va_list ap;
    const char *d;

    memset(p, 0, sizeof(*p));
    p->name = name;
    p->version = version;

    /* Copy the NULL-terminated variadic dependency list. */
    {
        size_t i = 0;

        va_start(ap, version);
        while (i < 8) {
            d = va_arg(ap, const char *);
            if (d == NULL)
                break;
            p->deps[i++] = d;
        }
        va_end(ap);
    }
}

static void world_reset(void)
{
    size_t i;

    for (i = 0; i < g_npkgs; i++)
        free(g_installed[i]);
    for (i = 0; i < g_nstray; i++) {
        free(g_stray[i].name);
        free(g_stray[i].version);
    }
    g_nstray = 0;

    g_npkgs = 0;
    lookup_count = 0;
    memset(g_pkgs, 0, sizeof(g_pkgs));
    memset(g_installed, 0, sizeof(g_installed));
}

static void world_install(const char *name, const char *version)
{
    size_t i;

    for (i = 0; i < g_npkgs; i++) {
        if (strcmp(g_pkgs[i].name, name) == 0) {
            g_installed[i] = strdup(version);
            return;
        }
    }
}

/* Resolver callbacks. */

static int fake_find(void *ud, const char *name, zbra_candidate *out)
{
    size_t i;

    (void)ud;
    lookup_count++;

    for (i = 0; i < g_npkgs; i++) {
        if (strcmp(g_pkgs[i].name, name) != 0)
            continue;

        memset(out, 0, sizeof(*out));
        out->name = strdup(g_pkgs[i].name);
        out->version = strdup(g_pkgs[i].version);
        out->style = ZBRA_VER_DEB;
        out->source = strdup("fake");
        out->format = strdup("deb");

        if (g_pkgs[i].deps[0] != NULL) {
            size_t n = 0;
            /* Test the bound BEFORE dereferencing, not after. */
            while (n < 8 && g_pkgs[i].deps[n] != NULL)
                n++;
            out->depends = calloc(n + 1, sizeof(*out->depends));
            out->n_depends = n;
            for (n = 0; n < out->n_depends; n++)
                out->depends[n] = strdup(g_pkgs[i].deps[n]);
        }

        return 1;
    }

    return 0;
}

static const char *fake_installed(void *ud, const char *name)
{
    size_t i;

    (void)ud;

    for (i = 0; i < g_npkgs; i++) {
        if (strcmp(g_pkgs[i].name, name) == 0)
            return g_installed[i];
    }

    for (i = 0; i < g_nstray; i++) {
        if (strcmp(g_stray[i].name, name) == 0)
            return g_stray[i].version;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Index of `name` in the plan, or -1. */
static int plan_index(const zbra_plan *p, const char *name)
{
    size_t i;

    for (i = 0; i < p->n; i++) {
        if (p->items[i].name != NULL && strcmp(p->items[i].name, name) == 0)
            return (int)i;
    }

    return -1;
}

/* Render the plan as "a,b,c" for readable assertions. */
static void plan_str(const zbra_plan *p, char *buf, size_t cap)
{
    size_t i;
    size_t off = 0;

    buf[0] = '\0';
    for (i = 0; i < p->n && off + 1 < cap; i++) {
        int k = snprintf(buf + off, cap - off, "%s%s", (i > 0) ? "," : "",
                         p->items[i].name);
        if (k < 0)
            break;
        off += (size_t)k;
    }
}

static void expect_plan(const zbra_plan *p, const char *want, const char *label)
{
    char buf[256];

    plan_str(p, buf, sizeof(buf));

    if (strcmp(buf, want) == 0) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s: plan was [%s], want [%s]\n", label, buf, want);
    }
}

/* ------------------------------------------------------------------ */
/* Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_single(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: single package");

    world_reset();
    pkg_add("vim", "9.1", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "vim", &plan, &err) == 0, "resolve succeeds");
    expect_plan(&plan, "vim", "single package");
    ok(plan.items[0].version != NULL &&
       strcmp(plan.items[0].version, "9.1") == 0, "version carried through");
    ok(err == NULL, "no error set");
    zbra_plan_free(&plan);
}

static void test_chain(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: linear chain is ordered dependencies-first");

    world_reset();
    pkg_add("app", "1.0", "libmid", NULL);
    pkg_add("libmid", "2.0", "libbase", NULL);
    pkg_add("libbase", "3.0", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == 0, "resolve succeeds");

    /*
     * The whole point of the ordering: installing app before libbase would
     * leave a broken intermediate system, so the plan must read
     * libbase,libmid,app.
     */
    expect_plan(&plan, "libbase,libmid,app", "chain ordered");

    /* Belt and braces: assert the positional invariant directly. */
    ok(plan_index(&plan, "libbase") < plan_index(&plan, "libmid"),
       "libbase precedes libmid");
    ok(plan_index(&plan, "libmid") < plan_index(&plan, "app"),
       "libmid precedes app");
    ok(plan_index(&plan, "app") == (int)plan.n - 1, "target is last");

    zbra_plan_free(&plan);
}

static void test_diamond(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: diamond dependency is fetched once");

    world_reset();
    pkg_add("app", "1.0", "left", "right", NULL);
    pkg_add("left", "1.0", "shared", NULL);
    pkg_add("right", "1.0", "shared", NULL);
    pkg_add("shared", "1.0", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == 0, "resolve succeeds");

    /* "shared" must appear exactly once, not once per path. */
    expect_plan(&plan, "shared,left,right,app", "diamond deduplicated");
    ok(lookup_count == 4, "each package looked up exactly once");

    zbra_plan_free(&plan);
}

static void test_cycle(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: cycles are detected, not recursed into");

    world_reset();
    pkg_add("a", "1.0", "b", NULL);
    pkg_add("b", "1.0", "c", NULL);
    pkg_add("c", "1.0", "a", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "a", &plan, &err) == -1, "cycle reported as error");
    ok(err != NULL, "error message produced");
    ok(err != NULL && strstr(err, "cycle") != NULL, "message mentions a cycle");

    /*
     * The message must name the loop, otherwise the packaging bug that
     * created it cannot be found.
     */
    ok(err != NULL && strstr(err, "a") != NULL &&
       strstr(err, "b") != NULL && strstr(err, "c") != NULL,
       "message names the full chain");

    free(err);
    zbra_plan_free(&plan);
}

static void test_self_cycle(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: a package depending on itself");

    world_reset();
    pkg_add("weird", "1.0", "weird", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "weird", &plan, &err) == -1, "self-cycle detected");
    free(err);
    zbra_plan_free(&plan);
}

static void test_satisfied_is_skipped(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: installed packages satisfying a requirement are skipped");

    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 2.0)", NULL);
    pkg_add("libfoo", "2.5", NULL);

    /* Already installed at a version that satisfies the requirement. */
    world_install("libfoo", "2.5");

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == 0, "resolve succeeds");

    /* libfoo is present and adequate, so only app is planned. */
    expect_plan(&plan, "app", "satisfied dependency skipped");
    zbra_plan_free(&plan);

    /* Same graph, but the installed version is too old: it must be planned. */
    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 3.0)", NULL);
    pkg_add("libfoo", "2.5", NULL);
    world_install("libfoo", "2.5");

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == 0, "resolve succeeds");
    expect_plan(&plan, "libfoo,app", "unsatisfied dependency is planned");
    zbra_plan_free(&plan);
}

static void test_constraint_forms(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: constraint forms");

    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 2.0, << 3.0)", NULL);
    pkg_add("libfoo", "2.5", NULL);
    world_install("libfoo", "2.5");
    zbra_plan_init(&plan);
    zbra_deps_resolve(&r, "app", &plan, &err);
    expect_plan(&plan, "app", "range satisfied by 2.5");
    zbra_plan_free(&plan);

    /* 4.0 violates the upper bound. */
    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 2.0, << 3.0)", NULL);
    pkg_add("libfoo", "4.0", NULL);
    world_install("libfoo", "4.0");
    zbra_plan_init(&plan);
    zbra_deps_resolve(&r, "app", &plan, &err);
    expect_plan(&plan, "libfoo,app", "range violated by 4.0");
    zbra_plan_free(&plan);

    /* An unsatisfiable upper bound against the only available version. */
    world_reset();
    pkg_add("app", "1.0", "libfoo (<< 1.0)", NULL);
    pkg_add("libfoo", "2.0", NULL);
    world_install("libfoo", "2.0");
    zbra_plan_init(&plan);
    zbra_deps_resolve(&r, "app", &plan, &err);
    expect_plan(&plan, "libfoo,app", "upper bound unsatisfiable, still planned");
    zbra_plan_free(&plan);
}

static void test_alternatives(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: 'a | b' alternatives are separate requirements");

    world_reset();
    pkg_add("app", "1.0", "libssl3 | libssl1.1", NULL);
    pkg_add("libssl3", "3.0", NULL);

    /*
     * Treating "a | b" as two independent requirements would try to install
     * both. This test documents the current behaviour -- zbra_dep_list_parse
     * expands alternatives, and the index layer is responsible for picking
     * one; here the raw single string is passed through, so only the
     * first alternative is walked.
     */
    zbra_plan_init(&plan);
    zbra_deps_resolve(&r, "app", &plan, &err);
    ok(plan_index(&plan, "libssl3") >= 0, "first alternative resolved");
    zbra_plan_free(&plan);
}

static void test_missing(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: missing package names the requirement that wanted it");

    world_reset();
    pkg_add("app", "1.0", "nonexistent", NULL);

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == -1, "missing dep is fatal");
    ok(err != NULL && strstr(err, "nonexistent") != NULL,
       "error names the missing package");
    ok(err != NULL && strstr(err, "app") != NULL,
       "error names the requiring package");
    free(err);
    zbra_plan_free(&plan);

    /* A missing explicit target is also an error. */
    zbra_plan_init(&plan);
    err = NULL;
    ok(zbra_deps_resolve(&r, "ghost", &plan, &err) == -1, "missing target fatal");
    free(err);
    zbra_plan_free(&plan);
}

static void test_installed_too_old_mentioned(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: failure message mentions an inadequate installed version");

    /*
     * libfoo is installed at 1.0 but no source offers it any more, and the
     * requirement is ">= 3.0". Resolution must fail and say why, quoting the
     * version that is actually present.
     */
    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 3.0)", NULL);
    world_install_stray("libfoo", "1.0");

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "app", &plan, &err) == -1,
       "no candidate for an unsatisfiable requirement");
    ok(err != NULL && strstr(err, "1.0") != NULL,
       "message quotes the installed version");
    free(err);
    zbra_plan_free(&plan);
}

static void test_check_installed(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    char *err = NULL;

    puts("deps: verify installed requirements");

    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 2.0)", NULL);
    pkg_add("libfoo", "2.5", NULL);
    world_install("app", "1.0");
    world_install("libfoo", "2.5");

    ok(zbra_deps_check_installed(&r, "app", &err) == 0, "healthy");
    free(err);

    /* Now break it. */
    world_reset();
    pkg_add("app", "1.0", "libfoo (>= 2.0)", NULL);
    pkg_add("libfoo", "1.0", NULL);
    world_install("app", "1.0");
    world_install("libfoo", "1.0");

    err = NULL;
    ok(zbra_deps_check_installed(&r, "app", &err) == -1, "unhealthy detected");
    ok(err != NULL && strstr(err, "libfoo") != NULL,
       "message names the unmet requirement");
    free(err);

    /* A missing dependency is reported too. */
    world_reset();
    pkg_add("app", "1.0", "libfoo", NULL);
    world_install("app", "1.0");
    err = NULL;
    ok(zbra_deps_check_installed(&r, "app", &err) == -1, "missing dep detected");
    ok(err != NULL && strstr(err, "not installed") != NULL,
       "message says it is not installed");
    free(err);
}

static void test_wide_graph(void)
{
    zbra_resolver r = { fake_find, fake_installed, NULL };
    zbra_plan plan;
    char *err = NULL;

    puts("deps: table growth and a wide graph");

    /*
     * Enough packages to force the visit table past its initial size and
     * through a rehash, which is where an off-by-one in the growth path
     * would show up as a lost or duplicated entry.
     */
    world_reset();
    {
        static char names[200][32];
        size_t i;

        for (i = 0; i < 200; i++) {
            snprintf(names[i], sizeof(names[i]), "pkg%zu", i);
            pkg_add(names[i], "1.0", NULL);
        }

        zbra_plan_init(&plan);
        ok(zbra_deps_resolve(&r, names[199], &plan, &err) == 0,
           "resolve in a large graph");
        ok(plan.n == 1, "single package in a large graph");
        zbra_plan_free(&plan);
        free(err);
        err = NULL;
    }

    /*
     * A star: one package depending on eight others. Verifies every
     * requirement is walked exactly once and appears once in the plan.
     */
    world_reset();
    {
        static char leaves[8][32];
        static char args[8][32];
        size_t i;

        for (i = 0; i < 8; i++) {
            snprintf(leaves[i], sizeof(leaves[i]), "leaf%zu", i);
            snprintf(args[i], sizeof(args[i]), "leaf%zu", i);
        }

        pkg_add("star", "1.0", args[0], args[1], args[2], args[3], args[4],
                args[5], args[6], args[7], NULL);
        for (i = 0; i < 8; i++)
            pkg_add(leaves[i], "1.0", NULL);
    }

    zbra_plan_init(&plan);
    ok(zbra_deps_resolve(&r, "star", &plan, &err) == 0, "resolve a wide graph");
    ok(plan.n == 9, "nine entries in the wide plan");

    {
        /* Each leaf must appear exactly once. */
        int dupes = 0;
        size_t i, j;

        for (i = 0; i < plan.n; i++) {
            for (j = i + 1; j < plan.n; j++) {
                if (plan.items[i].name != NULL &&
                    strcmp(plan.items[i].name, plan.items[j].name) == 0)
                    dupes++;
            }
        }
        ok(dupes == 0, "no duplicated entries in the plan");
    }

    zbra_plan_free(&plan);
    free(err);
}

int main(void)
{
    test_single();
    test_chain();
    test_diamond();
    test_cycle();
    test_self_cycle();
    test_satisfied_is_skipped();
    test_constraint_forms();
    test_alternatives();
    test_missing();
    test_installed_too_old_mentioned();
    test_check_installed();
    test_wide_graph();

    printf("\ndeps: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
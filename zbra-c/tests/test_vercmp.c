/*
 * test_vercmp.c -- unit tests for the version comparison routines.
 *
 * The expected orderings below were taken from dpkg's and rpm's own
 * documented behaviour rather than from this implementation, because a
 * version comparator that "agrees with itself" proves nothing. The
 * dpkg cases include the cases most implementations get wrong: tilde
 * ordering, numeric runs compared by magnitude rather than digit by digit,
 * and the asymmetric handling of letters versus punctuation.
 */

#include "vercmp.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

static void ok(int cond, const char *fmt, ...)
{
    (void)fmt;
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: ");
        printf(fmt);
        printf("\n");
    }
}

/* Assert a sign: negative when a < b, zero when equal, positive when a > b. */
static void expect_sign(const char *a, const char *b, int want_sign,
                        zbra_ver_style style, const char *label)
{
    int got = (style == ZBRA_VER_RPM) ? zbra_vercmp_rpm(a, b)
                                      : zbra_vercmp_deb(a, b);
    int sign = (got > 0) - (got < 0);

    if (sign == want_sign) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s: cmp(%s, %s) = %d, expected %s%s\n",
               label, a, b, got,
               want_sign < 0 ? "<" : (want_sign > 0 ? ">" : "=="),
               "");
    }
}

/*
 * Same assertion, but using the epoch/revision-aware comparator. The raw
 * zbra_vercmp_deb/rpm functions compare only the upstream part of a version
 * string, so anything involving an epoch or a Debian revision must go
 * through zbra_version_cmp instead.
 */
static void expect_ver(const char *a, const char *b, int want_sign,
                       zbra_ver_style style, const char *label)
{
    int got = zbra_version_cmp(a, b, style);
    int sign = (got > 0) - (got < 0);

    if (sign == want_sign) {
        g_pass++;
    } else {
        g_fail++;
        printf("  FAIL: %s: version_cmp(%s, %s) = %d, expected %s\n",
               label, a, b, got,
               want_sign < 0 ? "<" : (want_sign > 0 ? ">" : "=="));
    }
}

static void test_deb_basics(void)
{
    puts("vercmp: deb basics");
    expect_sign("1.0", "1.0",  0, ZBRA_VER_DEB, "identical");
    expect_sign("1.0", "1.1", -1, ZBRA_VER_DEB, "numeric run");
    expect_sign("1.1", "1.0",  1, ZBRA_VER_DEB, "numeric run reversed");
    expect_sign("1.0-1", "1.0-2", -1, ZBRA_VER_DEB, "revision");
    /* Verified with `dpkg --compare-versions 1.0-1 gt 1.0`. */
    expect_ver("1.0-1", "1.0",   1, ZBRA_VER_DEB, "revision present vs absent");
    expect_sign("2.0", "10.0", -1, ZBRA_VER_DEB, "numeric magnitude");
}

static void test_deb_tilde(void)
{
    puts("vercmp: deb tilde semantics");
    /* The whole reason tilde exists: pre-releases sort before the release. */
    expect_sign("1.0~rc1", "1.0",   -1, ZBRA_VER_DEB, "tilde < release");
    expect_sign("1.0", "1.0~rc1",   1, ZBRA_VER_DEB, "release > tilde");
    expect_sign("1.0~~", "1.0~",    -1, ZBRA_VER_DEB, "tilde sorts before tilde");
    expect_sign("1.0~beta1", "1.0~rc1", -1, ZBRA_VER_DEB, "tilde lexical (b < r)");
    expect_sign("1.0~rc1", "1.0~beta1", 1, ZBRA_VER_DEB, "tilde lexical reversed");
    /* A trailing tilde sorts before the empty string. */
    expect_sign("1.0~", "1.0",     -1, ZBRA_VER_DEB, "trailing tilde");
}

static void test_deb_numeric_runs(void)
{
    puts("vercmp: deb numeric runs compare by magnitude");
    /*
     * dpkg compares the whole numeric run, so 007 (== 7) is greater than 1.
     * An implementation that compares digit by digit would get this wrong.
     */
    expect_sign("1.007", "1.1",    1, ZBRA_VER_DEB, "007 > 1");
    expect_sign("1.0", "1.00",     0, ZBRA_VER_DEB, "leading zeros equal");
    expect_sign("1.010", "1.10",   0, ZBRA_VER_DEB, "leading zeros equal 2");
    expect_sign("1.9", "1.10",    -1, ZBRA_VER_DEB, "9 < 10");
}

static void test_deb_letters_vs_punct(void)
{
    puts("vercmp: deb letters sort before punctuation");
    /* In dpkg, letters rank below '+' and other symbols (c + 256). */
    expect_sign("1.0a", "1.0+b",   -1, ZBRA_VER_DEB, "letter before plus");
    expect_sign("1.0+b", "1.0a",   1, ZBRA_VER_DEB, "plus after letter");
    expect_sign("1.0a", "1.0b",    -1, ZBRA_VER_DEB, "letters lexical");
    /* Case sensitivity: uppercase sorts before lowercase by ASCII. */
    expect_sign("1.0A", "1.0a",    -1, ZBRA_VER_DEB, "case sensitive");
}

static void test_epoch(void)
{
    puts("vercmp: epoch");
    expect_ver("1:1.0", "2.0",   1, ZBRA_VER_DEB, "epoch dominates");
    expect_ver("1:1.0", "1:0.9", 1, ZBRA_VER_DEB, "same epoch");
    expect_ver("0:1.0", "1.0",   0, ZBRA_VER_DEB, "explicit zero epoch");
    expect_ver("2:0", "1:99",    1, ZBRA_VER_DEB, "epoch over upstream");
    expect_ver("1:1.0", "1.0",   1, ZBRA_VER_DEB, "epoch vs plain");
}

static void test_full_version_parse(void)
{
    puts("vercmp: epoch-aware full comparison");
    expect_ver("1:1.0-1", "1.0-2",  1, ZBRA_VER_DEB, "epoch beats revision");
    expect_ver("1.0-beta-3", "1.0-beta-4", -1, ZBRA_VER_DEB, "last hyphen splits");
    /*
     * Verified with `dpkg --compare-versions 1.0 eq 1.0-0`: dpkg treats an
     * absent revision as "0". rpm does NOT, which is why the default is
     * style-dependent in zbra_ver_compare().
     */
    expect_ver("1.0", "1.0-0",   0, ZBRA_VER_DEB, "absent revision == 0");
    expect_ver("1.0-0", "1.0",   0, ZBRA_VER_DEB, "absent revision == 0 (rev)");
    expect_ver("1.0", "1.0-1",  -1, ZBRA_VER_DEB, "1.0 < 1.0-1");
    expect_ver("1.0", "1.0-0",  -1, ZBRA_VER_RPM, "rpm absent revision sorts first");
}

static void test_rpm_basics(void)
{
    puts("vercmp: rpm basics");
    expect_sign("1.0", "1.0",     0, ZBRA_VER_RPM, "identical");
    expect_sign("1.0", "1.1",    -1, ZBRA_VER_RPM, "numeric");
    expect_sign("1.0-1", "1.0-2", -1, ZBRA_VER_RPM, "release");
    expect_sign("1.0", "1.0.1",  -1, ZBRA_VER_RPM, "extra segment is greater");
    expect_sign("1.0.1", "1.0",   1, ZBRA_VER_RPM, "extra segment reversed");
    expect_sign("2.0", "10.0",   -1, ZBRA_VER_RPM, "numeric magnitude");
}

static void test_rpm_segments(void)
{
    puts("vercmp: rpm segment handling");
    /* Separators are not compared; only segments are. */
    expect_sign("1.0-1", "1.0.1",  0, ZBRA_VER_RPM, "separator spelling ignored");
    /* Numeric segments strip leading zeros. */
    expect_sign("1.007", "1.7",   0, ZBRA_VER_RPM, "leading zeros");
    expect_sign("1.010", "1.10",  0, ZBRA_VER_RPM, "leading zeros 2");
    /* Mixed segments. */
    expect_sign("1.0rc1", "1.0",  1, ZBRA_VER_RPM, "trailing alpha greater");
    expect_sign("1.0a", "1.0b",  -1, ZBRA_VER_RPM, "alpha lexical");
}

static void test_rpm_operators(void)
{
    puts("vercmp: rpm tilde and caret");
    /* Tilde sorts before everything, including the end of the string. */
    expect_sign("1.0~rc1", "1.0",   -1, ZBRA_VER_RPM, "tilde < release");
    expect_sign("1.0~~", "1.0~",    -1, ZBRA_VER_RPM, "double tilde");
    /*
     * The critical case that motivated this rewrite: a tilde after a
     * separator must not be swallowed as a separator.
     */
    expect_sign("1.0-~rc1", "1.0", -1, ZBRA_VER_RPM, "tilde after separator");
    expect_sign("1.0-~rc1", "1.0-1", -1, ZBRA_VER_RPM, "tilde after separator 2");
    /* Caret: after the base, before any further segment. */
    expect_ver("2:1.0", "1:0.9",   1, ZBRA_VER_RPM, "rpm epoch");
    expect_ver("1.0^git1", "1.0",   1, ZBRA_VER_RPM, "caret > base");
    expect_sign("1.0^git1", "1.0.1", -1, ZBRA_VER_RPM, "caret < next segment");
    expect_sign("1.0", "1.0^2020",  -1, ZBRA_VER_RPM, "base < caret");
}

static void test_satisfies(void)
{
    puts("vercmp: constraint satisfaction");
    ok(zbra_ver_satisfies("1.2", ">= 1.0", ZBRA_VER_DEB) == 1, "1.2 >= 1.0");
    ok(zbra_ver_satisfies("0.9", ">= 1.0", ZBRA_VER_DEB) == 0, "0.9 not >= 1.0");
    ok(zbra_ver_satisfies("1.0", "<< 2.0", ZBRA_VER_DEB) == 1, "1.0 << 2.0");
    ok(zbra_ver_satisfies("2.0", "<< 1.0", ZBRA_VER_DEB) == 0, "2.0 not << 1.0");
    ok(zbra_ver_satisfies("1.0", "<= 1.0", ZBRA_VER_DEB) == 1, "1.0 <= 1.0");
    ok(zbra_ver_satisfies("1.0", "= 1.0", ZBRA_VER_DEB) == 1, "1.0 = 1.0");
    ok(zbra_ver_satisfies("1.0", "*", ZBRA_VER_DEB) == 1, "wildcard");
    /* A bare version means ">=", matching APT. */
    ok(zbra_ver_satisfies("1.0", "1.0", ZBRA_VER_DEB) == 1, "bare 1.0 >= 1.0");
    ok(zbra_ver_satisfies("0.9", "1.0", ZBRA_VER_DEB) == 0, "bare 0.9 >= 1.0 false");
    /* No whitespace variants. */
    ok(zbra_ver_satisfies("1.5", ">=1.0", ZBRA_VER_DEB) == 1, "no space after >=");
    ok(zbra_ver_satisfies("1.5", ">=  1.0", ZBRA_VER_DEB) == 1, "extra spaces");
    /* Alternatives are handled by the caller, not here. */
    ok(zbra_ver_satisfies("1.0", NULL, ZBRA_VER_DEB) == -1, "NULL constraint errors");
}

static void test_parse(void)
{
    puts("vercmp: parsing");
    zbra_ver v;

    ok(zbra_ver_parse("1:2.3-4", &v, ZBRA_VER_DEB) == 0, "parse ok");
    ok(v.epoch == 1, "epoch parsed");
    ok(v.upstream && strcmp(v.upstream, "2.3") == 0, "upstream parsed");
    ok(v.revision && strcmp(v.revision, "4") == 0, "revision parsed");
    zbra_ver_free(&v);

    ok(zbra_ver_parse("1.0", &v, ZBRA_VER_DEB) == 0, "parse no epoch");
    ok(v.epoch == 0, "default epoch 0");
    ok(v.revision == NULL, "no revision");
    zbra_ver_free(&v);

    ok(zbra_ver_parse("1.0-beta-3", &v, ZBRA_VER_DEB) == 0, "parse hyphenated");
    ok(v.upstream && strcmp(v.upstream, "1.0-beta") == 0, "last hyphen splits");
    ok(v.revision && strcmp(v.revision, "3") == 0, "revision from last hyphen");
    zbra_ver_free(&v);

    /* A colon that is not preceded by digits is not an epoch. */
    ok(zbra_ver_parse("1.2:3.4", &v, ZBRA_VER_DEB) == 0, "parse odd colon");
    ok(v.epoch == 0, "no epoch from dotted colon");
    zbra_ver_free(&v);
}

int main(void)
{
    test_deb_basics();
    test_deb_tilde();
    test_deb_numeric_runs();
    test_deb_letters_vs_punct();
    test_epoch();
    test_full_version_parse();
    test_rpm_basics();
    test_rpm_segments();
    test_rpm_operators();
    test_satisfies();
    test_parse();

    printf("\nvercmp: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
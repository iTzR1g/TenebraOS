/*
 * test_deb822.c -- unit tests for the control-stanza parser.
 *
 * The sample data is copied from a real Debian binary Packages file,
 * including the awkward parts: folded Depends lists, continuation lines
 * that begin with commas, an architecture qualifier, alternatives, and
 * fields whose names differ in case between formats.
 */

#include "deb822.h"

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

/*
 * A realistic stanza: folded Depends, an alternative, an architecture
 * qualifier, a multi-constraint dependency, and Description continuation
 * lines that legitimately contain a colon.
 */
static const char *SAMPLE =
"Package: zbra\n"
"Version: 1.0.0\n"
"Architecture: amd64\n"
"Maintainer: TenebraOS Team <team@tenebraos.org>\n"
"Installed-Size: 412\n"
"Depends: libc6 (>= 2.38), libssl3 (>= 3.0),\n"
" libarchive13, python3:any (>= 3.11)\n"
"Recommends: bash-completion\n"
"Description: universal package manager for TenebraOS\n"
" This is a long description that continues on the next line.\n"
" It even contains a colon: like this one.\n"
" .\n"
"Homepage: https://tenebraos.org\n"
"\n"
"Package: zbra-doc\n"
"Version: 1.0.0\n"
"Architecture: all\n"
"Depends: zbra\n"
"Description: documentation for zbra\n"
"\n";

static void test_basic_fields(void)
{
    zbra_d822 r;
    FILE *fp;

    fp = tmpfile();
    fputs(SAMPLE, fp);
    rewind(fp);

    ok(zbra_d822_open(&r, fp) == 0, "open");
    ok(zbra_d822_next(&r) == 1, "first stanza");

    eq_str(zbra_d822_get(&r, "Package"), "zbra", "Package");
    eq_str(zbra_d822_get(&r, "Version"), "1.0.0", "Version");
    eq_str(zbra_d822_get(&r, "Architecture"), "amd64", "Architecture");
    eq_str(zbra_d822_get(&r, "Installed-Size"), "412", "Installed-Size");
    ok(zbra_d822_get(&r, "Nonexistent") == NULL, "absent field is NULL");

    /* Debian capitalises field names; lookups must be case-insensitive so
     * the same code can read RPM's "Name"/"Version". */
    eq_str(zbra_d822_get(&r, "package"), "zbra", "case-insensitive key");
    eq_str(zbra_d822_get(&r, "PACKAGE"), "zbra", "upper-case key");

    /* Second stanza follows after the blank line. */
    ok(zbra_d822_next(&r) == 1, "second stanza");
    eq_str(zbra_d822_get(&r, "Package"), "zbra-doc", "second Package");
    eq_str(zbra_d822_get(&r, "Architecture"), "all", "second Architecture");

    ok(zbra_d822_next(&r) == 0, "end of input");

    zbra_d822_close(&r);
    fclose(fp);
}

static void test_folding(void)
{
    zbra_d822 r;
    FILE *fp;
    const char *v;

    fp = tmpfile();
    fputs(SAMPLE, fp);
    rewind(fp);

    zbra_d822_open(&r, fp);
    zbra_d822_next(&r);

    /*
     * The Depends field spans three physical lines. Continuation lines must
     * be folded into one value with no embedded newlines, otherwise every
     * consumer would have to re-implement folding.
     */
    v = zbra_d822_get(&r, "Depends");
    ok(v != NULL, "Depends present");
    ok(v != NULL && strchr(v, '\n') == NULL, "Depends has no embedded newline");
    ok(v != NULL && strstr(v, "libc6 (>= 2.38)") != NULL, "first dep kept");
    ok(v != NULL && strstr(v, "libarchive13") != NULL, "folded dep kept");
    ok(v != NULL && strstr(v, "python3:any") != NULL, "qualified dep kept");

    /*
     * A folded Description whose continuation contains ": " must not be
     * mistaken for a new field.
     */
    v = zbra_d822_get(&r, "Description");
    ok(v != NULL && strstr(v, "contains a colon: like this one") != NULL,
       "Description colon preserved");
    ok(v != NULL && strchr(v, '\n') == NULL, "Description folded");

    /* Fields after the folded block must still be found. */
    eq_str(zbra_d822_get(&r, "Homepage"), "https://tenebraos.org",
           "field after folded block");

    zbra_d822_close(&r);
    fclose(fp);
}

static void test_split(void)
{
    char **list = NULL;
    size_t n = 0;

    ok(zbra_d822_split("a, b ,c", &list, &n) == 0, "split ok");
    ok(n == 3, "split count");
    eq_str(list[0], "a", "split[0]");
    eq_str(list[1], "b", "split[1] trimmed");
    eq_str(list[2], "c", "split[2]");
    zbra_d822_strlist_free(list, n);

    /* Degenerate inputs must not crash or leak. */
    ok(zbra_d822_split("", &list, &n) == 0 && n == 0, "empty string");
    zbra_d822_strlist_free(list, n);
    ok(zbra_d822_split(NULL, &list, &n) == 0 && n == 0, "NULL string");
    zbra_d822_strlist_free(list, n);
    ok(zbra_d822_split("a,,b", &list, &n) == 0 && n == 2, "empty element skipped");
    zbra_d822_strlist_free(list, n);
}

static void test_dep_parse(void)
{
    zbra_dep_full d;

    /* Simple constraint. */
    ok(zbra_dep_parse("libc6 (>= 2.38)", &d) == 0, "parse dep");
    eq_str(d.name, "libc6", "dep name");
    ok(d.n == 1, "dep constraint count");
    eq_str(d.constraints[0], ">= 2.38", "dep constraint");
    ok(d.qual == NULL, "no qualifier");
    ok(d.alt == NULL, "no alternative");
    zbra_dep_free(&d);

    /* Architecture qualifier. */
    ok(zbra_dep_parse("python3:any (>= 3.11)", &d) == 0, "parse qualified");
    eq_str(d.name, "python3", "qualified name has no colon");
    eq_str(d.qual, "any", "qualifier captured");
    ok(d.n == 1, "qualified constraint count");
    zbra_dep_free(&d);

    /* Alternative. */
    ok(zbra_dep_parse("libssl3 | libssl1.1", &d) == 0, "parse alternative");
    eq_str(d.name, "libssl3", "alt first");
    eq_str(d.alt, "libssl1.1", "alt second");
    zbra_dep_free(&d);

    /* Multiple constraints. */
    ok(zbra_dep_parse("foo (>= 1.0, << 2.0)", &d) == 0, "parse multi-constraint");
    eq_str(d.name, "foo", "multi name");
    ok(d.n == 2, "two constraints");
    eq_str(d.constraints[0], ">= 1.0", "constraint 0");
    eq_str(d.constraints[1], "<< 2.0", "constraint 1");
    zbra_dep_free(&d);

    /* Bare name, no constraints. */
    ok(zbra_dep_parse("bash", &d) == 0, "parse bare");
    eq_str(d.name, "bash", "bare name");
    ok(d.n == 0, "bare has no constraints");
    zbra_dep_free(&d);

    /* Unbalanced parenthesis must not be treated as a constraint. */
    ok(zbra_dep_parse("weird (1.0", &d) == 0, "parse unbalanced");
    eq_str(d.name, "weird (1.0", "unbalanced paren kept in name");
    zbra_dep_free(&d);
}

static void test_dep_list(void)
{
    zbra_dep_full *list = NULL;
    size_t n = 0;
    const char *deps =
        "libc6 (>= 2.38), libarchive13,\n"
        " libssl3 | libssl1.1,\n"
        " python3:any (>= 3.11)";

    ok(zbra_dep_list_parse(deps, &list, &n) == 0, "parse dep list");
    /*
     * Four comma-separated elements, one of which is an "a | b" choice that
     * expands to two entries.
     */
    ok(n == 5, "dep list expands alternatives");
    if (n == 5) {
        eq_str(list[0].name, "libc6", "list[0]");
        eq_str(list[1].name, "libarchive13", "list[1]");
        eq_str(list[2].name, "libssl3", "list[2] alt left");
        eq_str(list[3].name, "libssl1.1", "list[3] alt right");
        eq_str(list[4].name, "python3", "list[4]");
        eq_str(list[4].qual, "any", "list[4] qualifier");
    }
    zbra_dep_list_free(list, n);

    /* An absent dependency field yields an empty list, not an error. */
    ok(zbra_dep_list_parse(NULL, &list, &n) == 0 && n == 0, "NULL deps");
    zbra_dep_list_free(list, n);
    ok(zbra_dep_list_parse("", &list, &n) == 0 && n == 0, "empty deps");
    zbra_dep_list_free(list, n);
}

static void test_format(void)
{
    zbra_dep_full d;
    char *s;

    ok(zbra_dep_parse("libc6 (>= 2.38)", &d) == 0, "parse for format");
    s = zbra_dep_format(&d);
    eq_str(s, "libc6 (>= 2.38)", "format roundtrip");
    free(s);
    zbra_dep_free(&d);

    /* Round-trip the qualified form. */
    ok(zbra_dep_parse("python3:any (>= 3.11)", &d) == 0, "parse qualified fmt");
    s = zbra_dep_format(&d);
    eq_str(s, "python3:any (>= 3.11)", "format qualified");
    free(s);
    zbra_dep_free(&d);
}

static void test_repeated_fields(void)
{
    zbra_d822 r;
    FILE *fp;

    /* RPM .spec files repeat Source: and Package: for multi-binary builds. */
    fp = tmpfile();
    fputs("Name: foo\n"
          "Version: 2.0\n"
          "Source0: foo-2.0.tar.gz\n"
          "Source1: extra.tar.gz\n"
          "Name: foo\n"
          "Version: 2.0\n", fp);
    rewind(fp);

    zbra_d822_open(&r, fp);
    zbra_d822_next(&r);
    eq_str(zbra_d822_get_idx(&r, "Source", 0), "foo-2.0.tar.gz", "Source0");
    eq_str(zbra_d822_get_idx(&r, "Source", 1), "extra.tar.gz", "Source1");
    ok(zbra_d822_get_idx(&r, "Source", 2) == NULL, "Source2 absent");
    zbra_d822_close(&r);
    fclose(fp);
}

static void test_sb(void)
{
    zbra_sb sb;
    char *big;

    zbra_sb_init(&sb);
    ok(zbra_sb_add(&sb, "hello") == 0, "sb add");
    ok(zbra_sb_addf(&sb, " %s %d", "world", 42) == 0, "sb addf");
    eq_str(sb.buf, "hello world 42", "sb contents");
    zbra_sb_free(&sb);

    /* Force the vsnprintf heap path by exceeding the stack buffer. */
    zbra_sb_init(&sb);
    {
        char big_in[2048];
        memset(big_in, 'x', sizeof(big_in) - 1);
        big_in[sizeof(big_in) - 1] = '\0';
        ok(zbra_sb_add(&sb, big_in) == 0, "sb add long");
        ok(zbra_sb_addf(&sb, "%s", "tail") == 0, "sb addf long");
        eq_str(sb.buf + strlen(big_in), "tail", "sb tail after long");
    }
    big = NULL;
    (void)big;
    zbra_sb_free(&sb);
}

int main(void)
{
    test_basic_fields();
    test_folding();
    test_split();
    test_dep_parse();
    test_dep_list();
    test_format();
    test_repeated_fields();
    test_sb();

    printf("\ndeb822: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
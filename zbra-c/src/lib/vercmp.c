/*
 * vercmp.c -- version comparison for zbra.
 *
 * See vercmp.h for the contract. The two comparison routines below are
 * ports of the reference implementations:
 *
 *   - zbra_vercmp_deb follows dpkg's verrevcmp(), which is a small
 *     character-ordering loop rather than a numeric/alpha split. The
 *     subtlety is that digits do not compare against each other inside
 *     the loop at all: they are consumed as a numeric run and compared by
 *     magnitude. '~' orders before every other character including the end
 *     of string, which is what gives pre-release ordering.
 *
 *   - zbra_vercmp_rpm follows rpm's rpmvercmp(), which splits both strings
 *     into alphanumeric segments and compares segment by segment. Numeric
 *     segments compare numerically, alphabetic segments lexically, and the
 *     '~'/'^' operators have the semantics rpm gives them.
 *
 * Both are implemented without allocation and without locale-sensitive
 * calls, so they behave identically on every machine regardless of the
 * environment's locale.
 */

#include "vercmp.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/*
 * Locale-independent character classification.
 *
 * <ctype.h> functions consult LC_CTYPE, so "isalpha" could be true for
 * bytes >= 0x80 under some locales. That would make version ordering depend
 * on the environment, which is unacceptable for a package manager: the same
 * Packages file must resolve identically on every machine. These helpers
 * restrict the tests to ASCII, which is what dpkg and rpm both assume.
 */
static inline int zbra_is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static inline int zbra_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static inline int zbra_is_alnum(int c)
{
    return zbra_is_digit(c) || zbra_is_alpha(c);
}

/* ------------------------------------------------------------------ */
/* Debian: dpkg verrevcmp                                              */
/* ------------------------------------------------------------------ */

/*
 * dpkg's per-character ordering used while walking two version strings.
 *
 *   digits  -> 0        (digits are compared as numeric runs, not as chars)
 *   letters -> the character itself, so 'A'..'Z' and 'a'..'z' sort before
 *              every other punctuation character
 *   '~'     -> -1       sorts before the end of the string
 *   other   -> c + 256  sorts after all letters
 *   NUL     -> 0
 *
 * The result is deliberately ordered rather than an arbitrary mapping: it
 * is what makes dpkg treat "1.0a" < "1.0+b" and "1.0~~" < "1.0~".
 */
int zbra_vercmp_order(int c)
{
    if (c >= '0' && c <= '9')
        return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
        return c;
    if (c == '~')
        return -1;
    if (c == '\0')
        return 0;
    return c + 256;
}

/* One comparison step over upstream/revision parts (no epoch handling). */
static int verrevcmp_part(const char *a, const char *b)
{
    /* When either pointer is NULL dpkg treats it as an empty string. */
    if (a == NULL)
        a = "";
    if (b == NULL)
        b = "";

    while (*a || *b) {
        int first_diff = 0;

        /*
         * Compare the non-digit prefix. dpkg loops while at least one side
         * still has a non-digit character, and exits as soon as both sides
         * are positioned on digits (or exhausted).
         */
        while ((*a && !zbra_is_digit(*a)) ||
               (*b && !zbra_is_digit(*b))) {
            int ac = zbra_vercmp_order((unsigned char)*a);
            int bc = zbra_vercmp_order((unsigned char)*b);

            if (ac != bc)
                return ac - bc;

            a++;
            b++;
        }

        /* Skip leading zeros on both sides before treating digits as a number. */
        while (*a == '0')
            a++;
        while (*b == '0')
            b++;

        /* Consume the numeric run, remembering how the leading digits differed. */
        while (zbra_is_digit(*a) && zbra_is_digit(*b)) {
            if (first_diff == 0)
                first_diff = *a - *b;
            a++;
            b++;
        }

        /*
         * If exactly one side still has digits, it has the longer number and
         * is therefore greater ("1.007" < "1.1" -> 7 vs 1 stops the run and
         * the remaining side decides).
         */
        if (zbra_is_digit(*a))
            return 1;
        if (zbra_is_digit(*b))
            return -1;

        if (first_diff != 0)
            return first_diff;
    }

    return 0;
}

int zbra_vercmp_deb(const char *a, const char *b)
{
    return verrevcmp_part(a, b);
}
/* ------------------------------------------------------------------ */
/* RPM: rpmvercmp                                                     */
/* ------------------------------------------------------------------ */

/*
 * Extract one alphanumeric segment, advancing *pp past it.
 *
 * rpm splits a version on every non-alphanumeric character and compares
 * only the resulting segments, so "1.0-1" and "1.0.1" differ in segment
 * count rather than in separator spelling.
 *
 * A segment is a run of digits OR a run of letters -- never a mix -- which
 * is how rpm keeps "1.0a" (numeric "0", alpha "a") comparable.
 *
 * IMPORTANT: '~' and '^' are NOT separators here. They are comparison
 * operators and are handled by the caller before segment extraction. This
 * function therefore stops at them rather than skipping over them; if it
 * skipped them, a version such as "1.0-~rc1" would lose its tilde and the
 * ordering would be wrong.
 *
 * Returns a pointer to the first character of the segment, or NULL when no
 * segment remains. *numeric is set to 1 for a numeric segment, 0 otherwise.
 */
static const char *rpm_segment(const char **pp, int *numeric)
{
    const char *p = *pp;
    const char *start;

    /* Skip any leading separators. */
    while (*p && !zbra_is_alnum(*p) && *p != '~' && *p != '^')
        p++;

    if (!zbra_is_alnum(*p)) {
        *pp = p;
        return NULL;
    }

    start = p;
    *numeric = zbra_is_digit(*p);

    while (zbra_is_alnum(*p) && (zbra_is_digit(*p) == *numeric))
        p++;

    *pp = p;
    return start;
}

int zbra_vercmp_rpm(const char *a, const char *b)
{
    const char *pa, *pb;

    if (a == NULL)
        a = "";
    if (b == NULL)
        b = "";

    /* An exact string match short-circuits everything, including the
     * separator and operator handling below. */
    if (strcmp(a, b) == 0)
        return 0;

    pa = a;
    pb = b;

    while (*pa || *pb) {
        const char *sa, *sb;
        size_t la, lb;
        int na = 0, nb = 0;
        int rc;

        /*
         * Skip separators, but stop at '~' and '^' so the operator checks
         * below see them.
         */
        while (*pa && !zbra_is_alnum(*pa) && *pa != '~' && *pa != '^')
            pa++;
        while (*pb && !zbra_is_alnum(*pb) && *pb != '~' && *pb != '^')
            pb++;

        /*
         * '~' sorts before everything, including the end of the string.
         * So "1.0~rc1" < "1.0" and "1.0~~" < "1.0~".
         */
        if (*pa == '~' || *pb == '~') {
            if (*pa != '~')
                return 1;
            if (*pb != '~')
                return -1;
            pa++;
            pb++;
            continue;
        }

        /*
         * '^' sorts after the base version but before any further segments.
         * The asymmetry is deliberate in rpm: an exhausted string loses to a
         * caret ("1.0" < "1.0^git1"), while a caret loses to a real segment
         * ("1.0^git1" < "1.0.1").
         */
        if (*pa == '^' || *pb == '^') {
            if (!*pa)
                return -1;
            if (!*pb)
                return 1;
            if (*pa != '^')
                return 1;
            if (*pb != '^')
                return -1;
            pa++;
            pb++;
            continue;
        }

        /* One side is exhausted: the remaining segments decide. */
        if (!(*pa && *pb))
            break;

        sa = rpm_segment(&pa, &na);
        sb = rpm_segment(&pb, &nb);

        /*
         * Both pointers currently sit on an alphanumeric character, so
         * segment extraction cannot fail here.
         */
        if (sa == NULL || sb == NULL)
            break;

        la = (size_t)(pa - sa);
        lb = (size_t)(pb - sb);

        if (na && nb) {
            /*
             * Numeric segments compare by magnitude. Leading zeros are
             * stripped first so "1.007" == "1.7", and the longer remaining
             * string is necessarily the larger number.
             */
            while (la > 1 && *sa == '0') { sa++; la--; }
            while (lb > 1 && *sb == '0') { sb++; lb--; }

            if (la != lb)
                rc = (la < lb) ? -1 : 1;
            else
                rc = memcmp(sa, sb, la);
            if (rc != 0)
                return rc < 0 ? -1 : 1;
        } else if (na != nb) {
            /*
             * One segment is numeric and the other alphabetic. rpm compares
             * the overlapping prefix first, then falls back to length, which
             * orders "1.0rc1" after "1.0".
             */
            size_t n = (la < lb) ? la : lb;
            rc = memcmp(sa, sb, n);
            if (rc != 0)
                return rc < 0 ? -1 : 1;
            if (la != lb)
                return (la < lb) ? -1 : 1;
        } else {
            /* Both alphabetic: lexical comparison, then length. */
            rc = memcmp(sa, sb, la < lb ? la : lb);
            if (rc != 0)
                return rc < 0 ? -1 : 1;
            if (la != lb)
                return (la < lb) ? -1 : 1;
        }
    }

    /*
     * Every segment matched. Whichever string still has characters left
     * carries additional segments and is therefore the greater version.
     * This also covers the case where only separators remain ("1.0-" vs
     * "1.0", which rpm treats as equal).
     */
    if (!*pa && !*pb)
        return 0;
    return *pa ? 1 : -1;
}
int zbra_vercmp(const char *a, const char *b, zbra_ver_style style)
{
    if (style == ZBRA_VER_RPM)
        return zbra_vercmp_rpm(a, b);
    return zbra_vercmp_deb(a, b);
}

/* ------------------------------------------------------------------ */
/* Epoch-aware comparison                                              */
/* ------------------------------------------------------------------ */

int zbra_ver_parse(const char *str, zbra_ver *out, zbra_ver_style style)
{
    const char *colon;
    char *dash;   /* points into out->upstream, which we own and modify */

    (void)style; /* both ecosystems share the same textual shape */

    if (out == NULL)
        return -1;

    out->epoch = 0;
    out->upstream = NULL;
    out->revision = NULL;

    if (str == NULL)
        str = "";

    /*
     * A leading integer followed by ':' is the epoch. Scanning stops at the
     * first non-digit so that "1.2:3.4" correctly has no epoch (the "1" is
     * followed by '.', not ':').
     */
    colon = strchr(str, ':');
    if (colon != NULL) {
        int all_digits = 1;
        for (const char *p = str; p < colon; p++) {
            if (!zbra_is_digit(*p)) {
                all_digits = 0;
                break;
            }
        }
        if (all_digits && colon != str) {
            char *end = NULL;
            long v = strtol(str, &end, 10);
            out->epoch = (int)v;
            str = colon + 1;
        }
    }

    out->upstream = strdup(str);
    if (out->upstream == NULL)
        return -1;

    /*
     * The revision is the text after the LAST hyphen. Using the last hyphen
     * (not the first) is what dpkg does, so "1.0-beta-3" yields
     * upstream="1.0-beta" and revision="3".
     */
    dash = strrchr(out->upstream, '-');
    if (dash != NULL) {
        *dash = '\0';
        out->revision = strdup(dash + 1);
        if (out->revision == NULL) {
            free(out->upstream);
            out->upstream = NULL;
            return -1;
        }
    }

    return 0;
}

void zbra_ver_free(zbra_ver *v)
{
    if (v == NULL)
        return;
    free(v->upstream);
    free(v->revision);
    v->upstream = NULL;
    v->revision = NULL;
}

int zbra_ver_compare(const zbra_ver *a, const zbra_ver *b, zbra_ver_style style)
{
    int rc;

    if (a == NULL || b == NULL)
        return (a == b) ? 0 : ((a == NULL) ? -1 : 1);

    /* Epoch dominates everything else in both ecosystems. */
    if (a->epoch != b->epoch)
        return (a->epoch < b->epoch) ? -1 : 1;

    rc = zbra_vercmp(a->upstream, b->upstream, style);
    if (rc != 0)
        return rc;

    /*
     * Revision comparison.
     *
     * dpkg treats an ABSENT Debian revision as "0", which is why
     * `dpkg --compare-versions 1.0 eq 1.0-0` succeeds. rpm has no such rule:
     * an absent release is simply the empty string, so "1.0" sorts before
     * "1.0-0" there. The default is therefore style-dependent -- getting this
     * wrong would silently change which version the resolver decides to
     * upgrade to.
     */
    {
        const char *ra = (a->revision != NULL) ? a->revision
                                               : ((style == ZBRA_VER_DEB) ? "0" : "");
        const char *rb = (b->revision != NULL) ? b->revision
                                               : ((style == ZBRA_VER_DEB) ? "0" : "");

        if (ra[0] == '\0' && rb[0] == '\0')
            return 0;
        return zbra_vercmp(ra, rb, style);
    }
}

int zbra_version_cmp(const char *a, const char *b, zbra_ver_style style)
{
    zbra_ver va, vb;
    int rc;

    if (zbra_ver_parse(a, &va, style) != 0)
        return strcmp(a ? a : "", b ? b : "");
    if (zbra_ver_parse(b, &vb, style) != 0) {
        zbra_ver_free(&va);
        return strcmp(a ? a : "", b ? b : "");
    }

    rc = zbra_ver_compare(&va, &vb, style);
    zbra_ver_free(&va);
    zbra_ver_free(&vb);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Constraint matching                                                 */
/* ------------------------------------------------------------------ */

int zbra_ver_satisfies(const char *version, const char *constraint,
                       zbra_ver_style style)
{
    char op[3] = {0, 0, 0};
    const char *want;
    int rc;

    if (constraint == NULL)
        return -1;

    /* Skip leading whitespace. */
    while (*constraint == ' ' || *constraint == '\t')
        constraint++;

    /* "*" means "any version". */
    if (strcmp(constraint, "*") == 0)
        return 1;

    /* Parse a leading operator if present. */
    if (constraint[0] == '<' && constraint[1] == '<') {
        op[0] = op[1] = '<';
        want = constraint + 2;
    } else if (constraint[0] == '>' && constraint[1] == '>') {
        op[0] = op[1] = '>';
        want = constraint + 2;
    } else if (constraint[0] == '<' && constraint[1] == '=') {
        op[0] = '<';
        op[1] = '=';
        want = constraint + 2;
    } else if (constraint[0] == '>' && constraint[1] == '=') {
        op[0] = '>';
        op[1] = '=';
        want = constraint + 2;
    } else if (constraint[0] == '<' || constraint[0] == '>' || constraint[0] == '=') {
        op[0] = constraint[0];
        want = constraint + 1;
    } else {
        /* Bare version: APT treats this as ">=". */
        op[0] = '>';
        op[1] = '=';
        want = constraint;
    }

    while (*want == ' ' || *want == '\t')
        want++;

    /* An empty version after the operator means "any", except for '<' forms
     * which cannot be satisfied by an unknown version. */
    if (*want == '\0') {
        if (op[0] == '>')
            return 1;   /* ">= nothing" is always true */
        if (op[0] == '=')
            return 1;   /* "= nothing" -- degenerate but treated as true */
        return -1;      /* "<< nothing" or "< nothing" is unsatisfiable */
    }

    rc = zbra_version_cmp(version, want, style);

    if (op[0] == '<') {
        if (op[1] == '=')
            return rc <= 0;
        return rc < 0;   /* "<<" means strictly less than */
    }
    if (op[0] == '>') {
        if (op[1] == '=')
            return rc >= 0;
        return rc > 0;   /* ">>" means strictly greater than */
    }
    /* '=' */
    return rc == 0;
}
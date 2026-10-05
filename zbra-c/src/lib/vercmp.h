/*
 * vercmp.h -- version comparison for zbra.
 *
 * zbra is a multi-format package manager, so it has to order versions from
 * more than one ecosystem correctly. Two algorithms are implemented:
 *
 *   ZBRA_VER_DEB  Debian's algorithm (dpkg verrevcmp). Handles the
 *                 "epoch:upstream-revision" triple and gives '~' its
 *                 documented meaning of "sorts before everything", which is
 *                 what makes 1.0~rc1 < 1.0. Used for .deb packages.
 *
 *   ZBRA_VER_RPM  RPM's algorithm (rpmvercmp). Segments are split on
 *                 non-alphanumeric boundaries, numeric segments compare
 *                 numerically (leading zeros ignored), alphabetic segments
 *                 compare lexically, and '~'/'^' carry tilde/caret
 *                 semantics. Used for .rpm packages pulled from DNF repos.
 *
 * Both entry points work on plain strings so callers that only need an
 * ordering never have to build a zbra_ver.
 */

#ifndef ZBRA_VERCMP_H
#define ZBRA_VERCMP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which ecosystem's ordering rules to apply. */
typedef enum {
    ZBRA_VER_DEB = 0,
    ZBRA_VER_RPM = 1
} zbra_ver_style;

/*
 * A split version. `epoch` is 0 when absent, `revision` is NULL when the
 * version carried none (for example "1.0" in Debian, or an rpm with no
 * release segment).
 */
typedef struct {
    int         epoch;
    char       *upstream;
    char       *revision;
} zbra_ver;

/*
 * Compare two upstream version strings using Debian ordering rules
 * (dpkg verrevcmp). Returns <0, 0 or >0 like strcmp. The arguments may be
 * NULL, which is treated as the empty string.
 */
int zbra_vercmp_deb(const char *a, const char *b);

/*
 * Compare two upstream version strings using RPM ordering rules. Same
 * return convention.
 */
int zbra_vercmp_rpm(const char *a, const char *b);

/* Style-dispatching convenience wrappers. */
int zbra_vercmp(const char *a, const char *b, zbra_ver_style style);

/*
 * Split "epoch:upstream-revision" into its parts. Returns 0 on success and
 * -1 if `str` is NULL. The result must be released with zbra_ver_free().
 *
 * Debian parses at most one colon; a colon inside the revision is not
 * special. RPM parsing is identical in shape, which is why one function
 * serves both styles.
 */
int zbra_ver_parse(const char *str, zbra_ver *out, zbra_ver_style style);

void zbra_ver_free(zbra_ver *v);

/*
 * Full comparison of two parsed versions, including epoch and revision.
 * Only the parts each style actually defines take part in the ordering:
 * the dpkg algorithm compares epoch, then upstream, then revision; RPM
 * compares epoch, then version, then release.
 */
int zbra_ver_compare(const zbra_ver *a, const zbra_ver *b, zbra_ver_style style);

/*
 * Parse two version strings and compare them. Convenient for callers that
 * have strings but need epoch handling. A NULL argument compares equal to
 * "0" / "1.0" respectively is NOT implied -- NULL is treated as empty.
 */
int zbra_version_cmp(const char *a, const char *b, zbra_ver_style style);

/*
 * Test a version against a single Debian-style relation: one of "<<",
 * "<=", "=", ">=", ">>" or the wildcard "*" (which accepts anything).
 * A constraint with no operator at all is treated as ">=", matching APT's
 * behaviour for a bare version in Depends.
 *
 * `version` is the candidate being tested, `constraint` the requirement.
 * Returns 1 when satisfied, 0 when not, -1 on a malformed constraint.
 */
int zbra_ver_satisfies(const char *version, const char *constraint,
                       zbra_ver_style style);

/* Exposed for testing: the character ordering dpkg applies when walking two
 * version strings. See the reference implementation in vercmp.c. */
int zbra_vercmp_order(int c);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_VERCMP_H */
/*
 * deps.h -- dependency resolution.
 *
 * Resolution answers one question: given a package the user asked for, what
 * else has to be installed, and in what order?
 *
 * The algorithm is a depth-first walk of the dependency graph producing a
 * topologically ordered list -- dependencies before dependents, the target
 * last -- which is exactly the sequence a package manager must apply,
 * because installing out of order is what produces broken intermediate
 * states.
 *
 * Three properties are non-negotiable and are all covered by tests:
 *
 *   Cycles are detected, not survived. A cycle would otherwise recurse until
 *   the stack ran out, and the message must name the full chain so the
 *   packaging bug that created it can actually be found.
 *
 *   Installed packages that already satisfy a requirement are skipped rather
 *   than reinstalled.
 *
 *   Lookups are memoised. A diamond dependency (A needs B and C, both need
 *   D) must fetch D once, not three times.
 *
 * The resolver knows nothing about where packages come from or how they are
 * unpacked. It asks callbacks, which the index and database layers provide.
 * That keeps this file testable in isolation and lets deb, rpm and tar.xz
 * payloads share one resolution strategy.
 */

#ifndef ZBRA_DEPS_H
#define ZBRA_DEPS_H

#include <stddef.h>

#include "vercmp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A package as offered by some source. */
typedef struct zbra_candidate {
    char            *name;
    char            *version;
    zbra_ver_style   style;        /* which ordering its version uses */
    char           **depends;      /* raw requirement strings */
    size_t           n_depends;
    char            *source;       /* which source provided it */
    char            *format;       /* "deb", "rpm", ... */
} zbra_candidate;

/* Release a candidate's heap members. */
void zbra_candidate_free(zbra_candidate *c);

/* Release an array of candidates. */
void zbra_candidate_list_free(zbra_candidate *list, size_t n);

/*
 * Callbacks the resolver needs from the rest of zbra.
 *
 * find_candidate  Provide the best available version of `name`.
 *                 Return 1 and fill *out (caller frees with
 *                 zbra_candidate_free), 0 if no source offers it, or -1.
 * installed_version
 *                 Return the installed version of `name`, or NULL when it is
 *                 not installed. The returned string stays owned by the
 *                 caller of the resolver.
 */
/*
 * How a caller answers the resolver's two questions.
 *
 * "What does this name resolve to?" wants the package index. "What is already
 * installed?" wants the database. Those are different objects with different
 * lifetimes, so they get different pointers: sharing one `ud` means every
 * caller has to cast, and a mismatched cast is a crash rather than a
 * compile error, which is how this got in here to begin with.
 */
typedef struct {
    /* Offered versions. May be NULL, which means nothing is available. */
    int          (*find_candidate)(void *ud, const char *name,
                                   zbra_candidate *out);
    void         *ud;

    /* Installed versions. May be NULL, which means nothing is installed. */
    const char *(*installed_version)(void *ud, const char *name);
    void         *ud_installed;
} zbra_resolver;

/* The ordered install plan. */
typedef struct {
    zbra_candidate *items;
    size_t          n;
    size_t          cap;
} zbra_plan;

void zbra_plan_init(zbra_plan *p);
void zbra_plan_free(zbra_plan *p);

/*
 * Resolve `target` into an ordered plan.
 *
 * On success returns 0 and fills *plan; the caller releases it with
 * zbra_plan_free. On failure returns -1 and stores a malloc'd explanation
 * in *err (release with free), naming the package that could not be
 * satisfied or the cycle that was found.
 *
 * When `deps_of` is NULL the target itself is the only entry, which is what
 * a caller wants for a package with no dependencies.
 */
int zbra_deps_resolve(const zbra_resolver *r, const char *target,
                      zbra_plan *plan, char **err);

/*
 * Verify that `requires` are all satisfied by what is installed right now.
 * Used by `zbra verify` and before an upgrade. Returns 0 when healthy, or -1
 * with *err naming the first unmet requirement.
 *
 * The requirement list is passed in rather than looked up because it has to
 * be the one that was recorded at install time. Reading it back out of the
 * index would report on whatever the repository says *now*: a package whose
 * source has disappeared, or whose new version dropped a dependency, would be
 * verified against somebody else's list. Neither answers "is this system
 * intact".
 */
int zbra_deps_check_installed(const zbra_resolver *r, const char *name,
                              const char *const *requires, size_t n_requires,
                              char **err);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_DEPS_H */
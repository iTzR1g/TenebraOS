/*
 * deps.c -- dependency resolution. See deps.h for the algorithm contract.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "deps.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deb822.h"   /* zbra_d822_split, zbra_dep_parse */

/* ------------------------------------------------------------------ */
/* Candidates                                                         */
/* ------------------------------------------------------------------ */

void zbra_candidate_free(zbra_candidate *c)
{
    if (c == NULL)
        return;

    free(c->name);
    free(c->version);
    free(c->source);
    free(c->format);
    zbra_d822_strlist_free(c->depends, c->n_depends);
    memset(c, 0, sizeof(*c));
}

void zbra_candidate_list_free(zbra_candidate *list, size_t n)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; i < n; i++)
        zbra_candidate_free(&list[i]);
    free(list);
}

void zbra_plan_init(zbra_plan *p)
{
    p->items = NULL;
    p->n = 0;
    p->cap = 0;
}

void zbra_plan_free(zbra_plan *p)
{
    if (p == NULL)
        return;
    zbra_candidate_list_free(p->items, p->n);
    p->items = NULL;
    p->n = 0;
    p->cap = 0;
}

/* Take ownership of a candidate into the plan. Returns 0 or -1 on OOM. */
static int plan_push(zbra_plan *p, zbra_candidate *c)
{
    if (p->n == p->cap) {
        size_t ncap = p->cap ? p->cap * 2 : 8;
        zbra_candidate *ni = realloc(p->items, ncap * sizeof(*ni));

        if (ni == NULL)
            return -1;

        p->items = ni;
        p->cap = ncap;
    }

    p->items[p->n++] = *c;
    memset(c, 0, sizeof(*c));
    return 0;
}

/* ------------------------------------------------------------------ */
/* A small string-keyed table                                         */
/* ------------------------------------------------------------------ */

/*
 * FNV-1a. Short, fast, and good enough to keep the linear probing below
 * from clustering badly on package names.
 */
static unsigned long str_hash(const char *s)
{
    unsigned long h = 2166136261UL;

    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }

    return h;
}

#define VISIT_EMPTY 0
#define VISIT_BUSY  1
#define VISIT_DONE  2

/* Marks one package's progress through the walk. */
typedef struct {
    char      *name;
    int        state;      /* VISIT_* */
    int        planned;    /* already appended to the plan */
    zbra_candidate cand;   /* fetched once, reused */
    int        have_cand;
} visit;

typedef struct {
    visit *slots;
    size_t cap;
    size_t used;
} visit_table;

static void vt_init(visit_table *t)
{
    t->slots = NULL;
    t->cap = 0;
    t->used = 0;
}

static void vt_free(visit_table *t)
{
    size_t i;

    if (t->slots == NULL)
        return;

    /* Only slots that were actually inserted own a name. */
    for (i = 0; i < t->cap; i++) {
        if (t->slots[i].name != NULL) {
            free(t->slots[i].name);
            if (t->slots[i].have_cand)
                zbra_candidate_free(&t->slots[i].cand);
        }
    }

    free(t->slots);
    t->slots = NULL;
    t->cap = 0;
    t->used = 0;
}

/*
 * Look up `name`, inserting a fresh EMPTY slot when absent. Returns NULL
 * only on allocation failure.
 */
static visit *vt_get(visit_table *t, const char *name, int *created)
{
    size_t idx;
    size_t i;

    *created = 0;

    if (t->cap == 0) {
        size_t ncap = 64;

        t->slots = calloc(ncap, sizeof(*t->slots));
        if (t->slots == NULL)
            return NULL;
        t->cap = ncap;
    }

    /* Grow when the table is three-quarters full to bound probe lengths. */
    if ((t->used + 1) * 4 >= t->cap * 3) {
        size_t ncap = t->cap * 2;
        visit *ns = calloc(ncap, sizeof(*ns));
        size_t k;

        if (ns == NULL)
            return NULL;

        for (k = 0; k < t->cap; k++) {
            if (t->slots[k].name != NULL) {
                size_t j = str_hash(t->slots[k].name) & (ncap - 1);
                while (ns[j].name != NULL)
                    j = (j + 1) & (ncap - 1);
                ns[j] = t->slots[k];
            }
        }

        free(t->slots);
        t->slots = ns;
        t->cap = ncap;
    }

    idx = str_hash(name) & (t->cap - 1);
    for (i = 0; i < t->cap; i++) {
        size_t j = (idx + i) & (t->cap - 1);

        if (t->slots[j].name == NULL) {
            t->slots[j].name = strdup(name);
            if (t->slots[j].name == NULL)
                return NULL;
            t->slots[j].state = VISIT_EMPTY;
            t->slots[j].planned = 0;
            t->slots[j].have_cand = 0;
            t->used++;
            *created = 1;
            return &t->slots[j];
        }

        if (strcmp(t->slots[j].name, name) == 0)
            return &t->slots[j];
    }

    return NULL;    /* table is full, which the growth check should prevent */
}

/* ------------------------------------------------------------------ */
/* Resolution                                                         */
/* ------------------------------------------------------------------ */

static char *errf(const char *fmt, ...)
{
    va_list ap;
    char stackbuf[1024];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return NULL;

    if ((size_t)n < sizeof(stackbuf))
        return strdup(stackbuf);

    {
        char *heap = malloc((size_t)n + 1);
        if (heap == NULL)
            return NULL;
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        return heap;
    }
}

/*
 * Render the current walk chain as "a -> b -> c" for cycle and failure
 * messages.
 */
static char *chain_str(char **chain, size_t n, const char *extra)
{
    zbra_sb sb;
    size_t i;

    zbra_sb_init(&sb);
    for (i = 0; i < n; i++) {
        if (i > 0)
            zbra_sb_add(&sb, " -> ");
        zbra_sb_add(&sb, chain[i]);
    }
    if (extra != NULL) {
        if (n > 0)
            zbra_sb_add(&sb, " -> ");
        zbra_sb_add(&sb, extra);
    }

    return sb.buf;      /* caller frees; NULL on OOM */
}

/*
 * Do the version CONSTRAINTS in `d` accept `version`?
 *
 * This judges version constraints only. It deliberately answers "yes" for an
 * empty constraint list, which is correct for that narrow question -- a bare
 * requirement like "libfoo" carries no version constraint -- but it is NOT a
 * statement that the requirement is met overall. Callers must separately
 * confirm the package is actually installed, otherwise a missing unversioned
 * dependency would be silently treated as satisfied.
 *
 * Returns 1 when every constraint accepts `version`, else 0.
 */
static int satisfies_all(const zbra_dep_full *d, const char *version,
                         zbra_ver_style style)
{
    size_t i;

    if (d->n == 0)
        return 1;               /* no version constraints to violate */
    if (version == NULL)
        return 0;               /* cannot satisfy constraints if absent */

    for (i = 0; i < d->n; i++) {
        if (!zbra_ver_satisfies(version, d->constraints[i], style))
            return 0;
    }

    return 1;
}

/*
 * Render a requirement's constraints as ">= 1.0, << 2.0", or NULL when
 * there are none.
 *
 * `d` is NULL when the node is the user's explicit target rather than some
 * other package's requirement, so the NULL case is normal and must not be
 * dereferenced.
 */
static char *constraints_text(const zbra_dep_full *d)
{
    zbra_sb sb;
    size_t i;

    if (d == NULL || d->n == 0)
        return NULL;

    zbra_sb_init(&sb);
    for (i = 0; i < d->n; i++) {
        if (i > 0)
            zbra_sb_add(&sb, ", ");
        zbra_sb_add(&sb, d->constraints[i]);
    }

    return sb.buf;
}

typedef struct {
    const zbra_resolver *r;
    zbra_plan           *plan;
    visit_table          vt;
    char               **chain;     /* current DFS path */
    size_t               chain_n;
    size_t               chain_cap;
    char                *err;
    int                  failed;
} walk_state;

static int chain_push(walk_state *w, const char *name)
{
    if (w->chain_n == w->chain_cap) {
        size_t ncap = w->chain_cap ? w->chain_cap * 2 : 16;
        char **nc = realloc(w->chain, ncap * sizeof(*nc));

        if (nc == NULL)
            return -1;

        w->chain = nc;
        w->chain_cap = ncap;
    }

    w->chain[w->chain_n] = strdup(name);
    if (w->chain[w->chain_n] == NULL)
        return -1;

    w->chain_n++;
    return 0;
}

static void chain_pop(walk_state *w)
{
    if (w->chain_n == 0)
        return;
    w->chain_n--;
    free(w->chain[w->chain_n]);
    w->chain[w->chain_n] = NULL;
}

/*
 * Walk one node. When `d` is non-NULL the node is being reached as a
 * requirement, so an already-installed version satisfying it is enough and
 * nothing is planned. When `d` is NULL the node is the user's explicit
 * target and is always planned.
 */
static int walk(walk_state *w, const char *name, const zbra_dep_full *d)
{
    visit *v;
    int created = 0;
    const char *inst;
    size_t i;

    v = vt_get(&w->vt, name, &created);
    if (v == NULL) {
        if (w->err == NULL)
            w->err = errf("out of memory while resolving %s", name);
        return -1;
    }

    /*
     * Reached while still on the stack: this is a cycle. Report the whole
     * chain, because the interesting part is the loop, not the last edge.
     */
    if (v->state == VISIT_BUSY) {
        char *cs = chain_str(w->chain, w->chain_n, name);

        if (w->err == NULL) {
            w->err = errf("dependency cycle detected: %s",
                          cs != NULL ? cs : name);
        }
        free(cs);
        return -1;
    }

    /* Already fully processed by an earlier branch. */
    if (v->state == VISIT_DONE)
        return 0;

    v->state = VISIT_BUSY;

    /*
     * An installed package that satisfies the requirement needs no work.
     * installed_version is optional: a caller with no database (building a
     * repository index, for instance) passes NULL.
     */
    inst = w->r->installed_version != NULL
               ? w->r->installed_version(w->r->ud_installed, name)
               : NULL;
    if (inst != NULL && d != NULL && satisfies_all(d, inst, ZBRA_VER_DEB)) {
        v->state = VISIT_DONE;
        return 0;
    }

    /* Fetch the candidate once; a diamond dependency must not re-fetch. */
    if (!v->have_cand) {
        int rc = w->r->find_candidate(w->r->ud, name, &v->cand);

        if (rc < 0) {
            if (w->err == NULL)
                w->err = errf("error while looking up %s", name);
            v->state = VISIT_EMPTY;
            return -1;
        }

        if (rc == 0) {
            /*
             * Nothing offers it. If a candidate was already staged by an
             * earlier branch of this walk, that is fine: the plan already
             * contains it even though the index no longer offers it (a
             * package being upgraded, or one only in a local source).
             */
            if (v->planned) {
                v->state = VISIT_DONE;
                return 0;
            }

            if (w->err == NULL) {
                char *cs = chain_str(w->chain, w->chain_n, NULL);
                char *ct = constraints_text(d);

                if (inst != NULL) {
                    w->err = errf(
                        "no package provides %s%s%s (installed %s does not "
                        "satisfy); required by %s",
                        name,
                        ct != NULL ? " " : "", ct != NULL ? ct : "",
                        inst,
                        (cs != NULL && cs[0] != '\0') ? cs : "the command line");
                } else {
                    w->err = errf("no package provides %s%s%s; required by %s",
                                  name,
                                  ct != NULL ? " " : "",
                                  ct != NULL ? ct : "",
                                  (cs != NULL && cs[0] != '\0') ? cs
                                                                  : "the command line");
                }

                free(cs);
                free(ct);
            }

            v->state = VISIT_EMPTY;
            return -1;
        }

        v->have_cand = 1;
    }

    /* Depth-first over this candidate's requirements. */
    if (v->cand.n_depends > 0) {
        for (i = 0; i < v->cand.n_depends; i++) {
            zbra_dep_full sub;

            if (zbra_dep_parse(v->cand.depends[i], &sub) != 0) {
                if (w->err == NULL)
                    w->err = errf("cannot parse requirement \"%s\" of %s",
                                  v->cand.depends[i], name);
                v->state = VISIT_EMPTY;
                return -1;
            }

            if (chain_push(w, sub.name) != 0) {
                zbra_dep_free(&sub);
                if (w->err == NULL)
                    w->err = errf("out of memory");
                v->state = VISIT_EMPTY;
                return -1;
            }

            if (walk(w, sub.name, &sub) != 0) {
                zbra_dep_free(&sub);
                chain_pop(w);
                v->state = VISIT_EMPTY;
                return -1;
            }

            chain_pop(w);
            zbra_dep_free(&sub);
        }
    }

    /* Everything this needs is now resolved, so it can go into the plan. */
    if (!v->planned) {
        if (plan_push(w->plan, &v->cand) != 0) {
            if (w->err == NULL)
                w->err = errf("out of memory building the install plan");
            v->state = VISIT_EMPTY;
            return -1;
        }
        v->planned = 1;
        /* plan_push took the strings; release our copy of the struct only. */
        v->have_cand = 0;
    }

    v->state = VISIT_DONE;
    return 0;
}

int zbra_deps_resolve(const zbra_resolver *r, const char *target,
                      zbra_plan *plan, char **err)
{
    walk_state w;
    int rc;

    if (err != NULL)
        *err = NULL;

    if (r == NULL || target == NULL || plan == NULL)
        return -1;

    memset(&w, 0, sizeof(w));
    w.r = r;
    w.plan = plan;
    vt_init(&w.vt);

    rc = chain_push(&w, target);
    if (rc != 0) {
        if (err != NULL)
            *err = errf("out of memory");
        goto out;
    }

    /*
     * The target is passed with a NULL requirement, which is what makes it
     * unconditional: an explicit request always results in something being
     * planned, even if the package is already installed (that is an upgrade
     * decision for the caller, not the resolver's).
     */
    rc = walk(&w, target, NULL);

out:
    /*
     * Hand the message to the caller by transferring ownership, and NULL the
     * local first -- otherwise the free() below would hand back a dangling
     * pointer to the caller to read.
     */
    if (rc != 0 && err != NULL && *err == NULL) {
        *err = (w.err != NULL) ? w.err : strdup("dependency resolution failed");
        w.err = NULL;
    }

    free(w.err);
    w.err = NULL;

    chain_pop(&w);
    {
        size_t i;
        for (i = 0; i < w.chain_n; i++)
            free(w.chain[i]);
    }
    free(w.chain);
    vt_free(&w.vt);

    return rc;
}

int zbra_deps_check_installed(const zbra_resolver *r, const char *name,
                              const char *const *requires, size_t n_requires,
                              char **err)
{
    size_t i;

    if (err != NULL)
        *err = NULL;

    if (r == NULL || name == NULL)
        return -1;

    if (requires == NULL || n_requires == 0)
        return 0;

    if (r->installed_version == NULL) {
        if (err != NULL)
            *err = errf("cannot check %s: no way to see what is installed",
                        name);
        return -1;
    }

    for (i = 0; i < n_requires; i++) {
        zbra_dep_full d;
        const char *inst;

        if (requires[i] == NULL)
            continue;

        if (zbra_dep_parse(requires[i], &d) != 0) {
            if (err != NULL)
                *err = errf("cannot parse requirement \"%s\" of %s",
                            requires[i], name);
            return -1;
        }

        inst = r->installed_version(r->ud_installed, d.name);

        /*
         * An absent package fails the requirement regardless of constraints:
         * "Depends: libfoo" is not met by having no libfoo at all.
         */
        if (inst == NULL || !satisfies_all(&d, inst, ZBRA_VER_DEB)) {
            char *ct = constraints_text(&d);

            if (err != NULL) {
                if (inst != NULL)
                    *err = errf("%s requires %s%s%s but %s is installed",
                                name, d.name,
                                ct != NULL ? " " : "",
                                ct != NULL ? ct : "", inst);
                else
                    *err = errf("%s requires %s%s%s which is not installed",
                                name, d.name,
                                ct != NULL ? " " : "",
                                ct != NULL ? ct : "");
            }

            free(ct);
            zbra_dep_free(&d);
            return -1;
        }

        zbra_dep_free(&d);
    }

    return 0;
}

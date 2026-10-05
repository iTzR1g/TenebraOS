/*
 * deb822.c -- parser for RFC822-style control stanzas. See deb822.h.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "deb822.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* String buffer                                                      */
/* ------------------------------------------------------------------ */

void zbra_sb_init(zbra_sb *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void zbra_sb_free(zbra_sb *sb)
{
    if (sb == NULL)
        return;
    free(sb->buf);
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(zbra_sb *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    size_t cap;
    char *nb;

    if (need <= sb->cap)
        return 0;

    cap = sb->cap ? sb->cap : 128;
    while (cap < need)
        cap *= 2;

    nb = realloc(sb->buf, cap);
    if (nb == NULL)
        return -1;

    sb->buf = nb;
    sb->cap = cap;
    return 0;
}

int zbra_sb_add(zbra_sb *sb, const char *s)
{
    size_t n;

    if (s == NULL)
        return 0;

    n = strlen(s);
    if (sb_reserve(sb, n) != 0)
        return -1;

    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
    return 0;
}

int zbra_sb_addf(zbra_sb *sb, const char *fmt, ...)
{
    va_list ap;
    int n;
    char stackbuf[512];

    va_start(ap, fmt);
    n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);

    if (n < 0)
        return -1;

    if ((size_t)n < sizeof(stackbuf))
        return zbra_sb_add(sb, stackbuf);

    /* The formatted output did not fit the stack buffer; size it exactly. */
    {
        char *heap = malloc((size_t)n + 1);
        int rc;

        if (heap == NULL)
            return -1;

        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);

        rc = zbra_sb_add(sb, heap);
        free(heap);
        return rc;
    }
}

/* ------------------------------------------------------------------ */
/* Reader                                                             */
/* ------------------------------------------------------------------ */

static void free_fields(zbra_d822_field *f)
{
    while (f != NULL) {
        zbra_d822_field *next = f->next;
        free(f->key);
        free(f->val);
        free(f);
        f = next;
    }
}

void zbra_d822_reset(zbra_d822 *r)
{
    if (r == NULL)
        return;
    free_fields(r->head);
    r->head = NULL;
    r->tail = NULL;
}

int zbra_d822_open(zbra_d822 *r, FILE *fp)
{
    if (r == NULL || fp == NULL)
        return -1;

    memset(r, 0, sizeof(*r));
    r->fp = fp;
    r->cap = 256;
    r->line = malloc(r->cap);
    if (r->line == NULL)
        return -1;

    r->lineno = 0;
    r->eof = 0;
    return 0;
}

void zbra_d822_close(zbra_d822 *r)
{
    if (r == NULL)
        return;
    free_fields(r->head);
    r->head = r->tail = NULL;
    free(r->line);
    r->line = NULL;
}

/* Strip a trailing newline, and a trailing CR if the file is CRLF. */
static void chomp(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/* Whitespace-trimmed copy; see the note on is_ws about embedded newlines. */
static char *trim_dup(const char *s, size_t len);

static int is_blank(const char *s)
{
    while (is_ws((unsigned char)*s))
        s++;
    return *s == '\0';
}

/*
 * Copy `len` bytes of `s`, stripping leading and trailing whitespace.
 *
 * Newlines and carriage returns are stripped as well as spaces and tabs.
 * Normally the deb822 reader has already folded continuations into a single
 * line, but callers also pass values straight out of control files and hand-
 * written sources.list fragments, where an embedded newline is possible.
 * Leaving it in place would silently corrupt the first character of a
 * package name.
 */
static char *trim_dup(const char *s, size_t len)
{
    char *out;

    while (len > 0 && is_ws((unsigned char)*s)) {
        s++;
        len--;
    }
    while (len > 0 && is_ws((unsigned char)s[len - 1]))
        len--;

    out = strndup(s, len);
    return out;
}

/*
 * Append a folded continuation line to the previous field's value.
 *
 * A Debian continuation line starts with a space/tab and the leading
 * whitespace is not part of the value. Because a folded list separates
 * elements with a comma that may sit at the start of the continuation, the
 * newline is replaced with a single space rather than being dropped, which
 * keeps "Depends: a,\n b" from becoming "a,b" ambiguously in the presence
 * of alternatives.
 */
static int append_continuation(zbra_d822_field *f, const char *text)
{
    zbra_sb sb;
    int rc;

    if (f == NULL)
        return -1;

    zbra_sb_init(&sb);
    if (zbra_sb_add(&sb, f->val) != 0 || zbra_sb_add(&sb, " ") != 0 ||
        zbra_sb_add(&sb, text) != 0) {
        zbra_sb_free(&sb);
        return -1;
    }

    rc = 0;
    free(f->val);
    f->val = sb.buf;                /* ownership transfers */
    if (f->val == NULL)
        rc = -1;

    return rc;
}

static int push_field(zbra_d822 *r, const char *key, size_t keylen,
                      const char *val)
{
    zbra_d822_field *f = calloc(1, sizeof(*f));

    if (f == NULL)
        return -1;

    f->key = strndup(key, keylen);
    f->val = strdup(val != NULL ? val : "");

    if (f->key == NULL || f->val == NULL) {
        free(f->key);
        free(f->val);
        free(f);
        return -1;
    }

    if (r->tail != NULL)
        r->tail->next = f;
    else
        r->head = f;
    r->tail = f;

    return 0;
}

int zbra_d822_next(zbra_d822 *r)
{
    int saw_any = 0;

    if (r == NULL || r->fp == NULL)
        return -1;

    zbra_d822_reset(r);

    for (;;) {
        ssize_t got;

        got = getline(&r->line, &r->cap, r->fp);
        if (got < 0) {
            if (ferror(r->fp))
                return -1;
            r->eof = 1;
            /* A trailing stanza not followed by a blank line still counts. */
            return (saw_any && r->head != NULL) ? 1 : 0;
        }

        r->lineno++;
        chomp(r->line);

        /* Blank line: end of stanza. */
        if (is_blank(r->line)) {
            if (r->head != NULL)
                return 1;
            continue;               /* skip runs of blank lines */
        }

        /* Continuation: leading space or tab. */
        if (r->line[0] == ' ' || r->line[0] == '\t') {
            char *text = r->line;

            while (*text == ' ' || *text == '\t')
                text++;

            if (r->tail == NULL) {
                /* A continuation with no preceding field is malformed; treat
                 * the whole line as a field so parsing can continue rather
                 * than aborting the entire index. */
                if (push_field(r, r->line, strlen(r->line), "") != 0)
                    return -1;
            } else if (append_continuation(r->tail, text) != 0) {
                return -1;
            }

            saw_any = 1;
            continue;
        }

        /* "Field: value" -- the colon may be absent in some legacy files. */
        {
            char *colon = strchr(r->line, ':');
            const char *val = "";
            size_t keylen;

            if (colon != NULL) {
                keylen = (size_t)(colon - r->line);
                val = colon + 1;
                while (*val == ' ' || *val == '\t')
                    val++;
            } else {
                keylen = strlen(r->line);
            }

            if (push_field(r, r->line, keylen, val) != 0)
                return -1;
            saw_any = 1;
        }
    }
}

static int ci_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (tolower(ca) != tolower(cb))
            return 0;
    }

    return *a == *b;
}

const char *zbra_d822_get(const zbra_d822 *r, const char *key)
{
    const zbra_d822_field *f;

    if (r == NULL || key == NULL)
        return NULL;

    for (f = r->head; f != NULL; f = f->next) {
        if (ci_equal(f->key, key))
            return f->val;
    }

    return NULL;
}

int zbra_d822_has(const zbra_d822 *r, const char *key)
{
    return zbra_d822_get(r, key) != NULL;
}

/*
 * Look up an RPM .spec style indexed field: key="Source", idx=2 finds the
 * field literally named "Source2". These names cannot be matched by
 * zbra_d822_getn, which compares the whole key for equality.
 */
const char *zbra_d822_get_idx(const zbra_d822 *r, const char *key, size_t idx)
{
    char want[128];
    const zbra_d822_field *f;

    if (r == NULL || key == NULL)
        return NULL;

    snprintf(want, sizeof(want), "%s%zu", key, idx);

    for (f = r->head; f != NULL; f = f->next) {
        if (ci_equal(f->key, want))
            return f->val;
    }

    return NULL;
}

const char *zbra_d822_getn(const zbra_d822 *r, const char *key, size_t idx)
{
    const zbra_d822_field *f;
    size_t i = 0;

    if (r == NULL || key == NULL)
        return NULL;

    for (f = r->head; f != NULL; f = f->next) {
        if (ci_equal(f->key, key)) {
            if (i == idx)
                return f->val;
            i++;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* List splitting                                                     */
/* ------------------------------------------------------------------ */



int zbra_d822_split(const char *value, char ***out, size_t *n)
{
    char **list = NULL;
    size_t count = 0;
    size_t cap = 0;
    const char *p;

    *out = NULL;
    *n = 0;

    /* An absent field means "nothing to split", which is not an error. */
    if (value == NULL)
        return 0;

    p = value;
    for (;;) {
        const char *comma = strchr(p, ',');
        size_t len = (comma != NULL) ? (size_t)(comma - p) : strlen(p);
        char *item;

        /* Skip leading whitespace left over from a folded continuation. */
        while (len > 0 && is_ws((unsigned char)*p)) {
            p++;
            len--;
        }

        item = trim_dup(p, len);
        if (item == NULL)
            goto fail;

        /* An empty element means a trailing comma; ignore it. */
        if (item[0] == '\0') {
            free(item);
        } else {
            if (count == cap) {
                char **nl;
                cap = cap ? cap * 2 : 8;
                nl = realloc(list, cap * sizeof(*list));
                if (nl == NULL) {
                    free(item);
                    goto fail;
                }
                list = nl;
            }
            list[count++] = item;
        }

        if (comma == NULL)
            break;
        p = comma + 1;
    }

    *out = list;
    *n = count;
    return 0;

fail:
    zbra_d822_strlist_free(list, count);
    return -1;
}

void zbra_d822_strlist_free(char **list, size_t n)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; i < n; i++)
        free(list[i]);
    free(list);
}

/* ------------------------------------------------------------------ */
/* Dependency parsing                                                 */
/* ------------------------------------------------------------------ */

/*
 * Parse the parenthesised constraint list that follows a package name:
 *
 *   "(>= 1.0, << 2.0)"  ->  {">= 1.0", "<< 2.0"}
 *
 * A version may legitimately contain commas only in exotic epoch forms, and
 * Debian never emits them inside a relation, so splitting on ',' is safe.
 */
static int parse_constraints(const char *text, zbra_dep_full *d)
{
    char **list = NULL;
    size_t n = 0;
    size_t i;
    char **dup = NULL;
    size_t dcount = 0;
    int rc = -1;

    if (zbra_d822_split(text, &list, &n) != 0)
        return -1;

    dup = calloc(n ? n : 1, sizeof(*dup));
    if (dup == NULL)
        goto out;

    for (i = 0; i < n; i++) {
        dup[i] = strdup(list[i]);
        if (dup[i] == NULL)
            goto out;
        dcount++;
    }

    free(d->constraints);
    d->constraints = dup;
    d->n = dcount;
    dup = NULL;
    rc = 0;

out:
    if (dup != NULL) {
        for (i = 0; i < dcount; i++)
            free(dup[i]);
        free(dup);
    }
    zbra_d822_strlist_free(list, n);
    return rc;
}

int zbra_dep_parse(const char *text, zbra_dep_full *out)
{
    char *work = NULL;
    char *namepart;
    char *paren;
    char *bar;
    char *colon;
    int rc = -1;

    if (text == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));

    work = trim_dup(text, strlen(text));
    if (work == NULL)
        return -1;

    /* Split off the '|' alternative first, so it cannot confuse the rest. */
    bar = strchr(work, '|');
    if (bar != NULL) {
        *bar = '\0';
        out->alt = trim_dup(bar + 1, strlen(bar + 1));
        if (out->alt == NULL)
            goto out;
    }

    /* Extract and remove a trailing "(...)" constraint group. */
    paren = strrchr(work, '(');
    if (paren != NULL) {
        char *close = strchr(paren, ')');

        if (close != NULL) {
            *close = '\0';
            if (parse_constraints(paren + 1, out) != 0)
                goto out;
            *paren = '\0';
        }
        /* A '(' with no ')' is not a constraint; leave the text alone. */
    }

    /* Peel an architecture qualifier such as ":any" or ":native". */
    namepart = trim_dup(work, strlen(work));
    if (namepart == NULL)
        goto out;

    colon = strrchr(namepart, ':');
    if (colon != NULL) {
        *colon = '\0';
        out->qual = strdup(colon + 1);
        if (out->qual == NULL) {
            free(namepart);
            goto out;
        }
    }

    out->name = namepart;
    rc = 0;

out:
    free(work);
    if (rc != 0)
        zbra_dep_free(out);
    return rc;
}

void zbra_dep_free(zbra_dep_full *d)
{
    if (d == NULL)
        return;

    free(d->name);
    free(d->qual);
    free(d->alt);
    zbra_d822_strlist_free(d->constraints, d->n);
    memset(d, 0, sizeof(*d));
}

int zbra_dep_list_parse(const char *value, zbra_dep_full **out, size_t *n)
{
    char **items = NULL;
    size_t n_in = 0;        /* number of comma-separated input elements */
    size_t n_out = 0;       /* number of dependency entries produced */
    zbra_dep_full *list = NULL;
    size_t i;
    int rc = -1;

    *out = NULL;
    *n = 0;

    if (value == NULL || is_blank(value))
        return 0;                  /* an absent field means no dependencies */

    if (zbra_d822_split(value, &items, &n_in) != 0)
        return -1;

    /*
     * Worst case each element expands into two entries: "a | b" becomes a
     * and b. Allocating only n_in slots would overflow the array as soon as
     * one alternative appeared, so reserve double up front.
     *
     * n_in and n_out are kept strictly separate: n_in bounds the loop over
     * the input array, while n_out tracks how many entries have been written
     * to the (separately allocated) output array. Sharing one counter would
     * make the loop read past the end of `items`.
     */
    list = calloc(n_in ? n_in * 2 : 1, sizeof(*list));
    if (list == NULL)
        goto out;

    for (i = 0; i < n_in; i++) {
        char *bar = strchr(items[i], '|');

        if (bar != NULL) {
            char *left, *right;

            *bar = '\0';
            left = trim_dup(items[i], (size_t)(bar - items[i]));
            right = trim_dup(bar + 1, strlen(bar + 1));

            if (left == NULL || right == NULL) {
                free(left);
                free(right);
                goto out;
            }

            if (zbra_dep_parse(left, &list[n_out]) != 0) {
                free(left);
                free(right);
                goto out;
            }
            free(left);
            n_out++;

            if (zbra_dep_parse(right, &list[n_out]) != 0) {
                free(right);
                goto out;
            }
            free(right);
            n_out++;
        } else {
            if (zbra_dep_parse(items[i], &list[n_out]) != 0)
                goto out;
            n_out++;
        }
    }

    *out = list;
    *n = n_out;
    list = NULL;
    rc = 0;

out:
    if (list != NULL)
        zbra_dep_list_free(list, n_out);
    zbra_d822_strlist_free(items, n_in);
    return rc;
}

void zbra_dep_list_free(zbra_dep_full *list, size_t n)
{
    size_t i;

    if (list == NULL)
        return;

    for (i = 0; i < n; i++)
        zbra_dep_free(&list[i]);
    free(list);
}

char *zbra_dep_format(const zbra_dep_full *d)
{
    zbra_sb sb;
    size_t i;

    if (d == NULL || d->name == NULL)
        return NULL;

    zbra_sb_init(&sb);

    if (zbra_sb_add(&sb, d->name) != 0)
        goto fail;

    if (d->qual != NULL && zbra_sb_addf(&sb, ":%s", d->qual) != 0)
        goto fail;

    if (d->n > 0) {
        if (zbra_sb_add(&sb, " (") != 0)
            goto fail;
        for (i = 0; i < d->n; i++) {
            if (i > 0 && zbra_sb_add(&sb, ", ") != 0)
                goto fail;
            if (zbra_sb_add(&sb, d->constraints[i]) != 0)
                goto fail;
        }
        if (zbra_sb_add(&sb, ")") != 0)
            goto fail;
    }

    if (d->alt != NULL && zbra_sb_addf(&sb, " | %s", d->alt) != 0)
        goto fail;

    return sb.buf;

fail:
    zbra_sb_free(&sb);
    return NULL;
}
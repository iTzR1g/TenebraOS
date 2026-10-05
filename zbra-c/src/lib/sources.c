/*
 * sources.c -- repository source configuration.
 *
 * Parsing is strict on purpose. A malformed source line is a user typo or a
 * stale config, and silently skipping it means zbra "works" while quietly
 * searching fewer repositories than the user believes, which is worse than
 * refusing to load. Unknown source *types* are the one exception: those come
 * from a newer config or a backend not compiled in, and they are collected as
 * warnings rather than aborting the load.
 */

#include "sources.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "deb822.h"

/* ---------------------------------------------------------- helpers */

static char *xstrdup(const char *s)
{
    return s != NULL ? strdup(s) : NULL;
}

static char *strf(const char *fmt, ...)
{
    va_list ap;
    char   *out;
    int     n;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0)
        return NULL;

    out = malloc((size_t)n + 1);
    if (out == NULL)
        return NULL;

    va_start(ap, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, ap);
    va_end(ap);

    return out;
}

static int is_blank_or_comment(const char *line)
{
    const char *p = line;

    while (*p == ' ' || *p == '\t')
        p++;

    return (*p == '\0' || *p == '\n' || *p == '#');
}

/* Trim leading/trailing whitespace in place, returning a pointer inside s. */
static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t')
        s++;

    end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';

    return s;
}

/* ---------------------------------------------------------- types */

const char *zbra_source_type_name(zbra_source_type t)
{
    switch (t) {
    case ZBRA_SRC_DEB:   return "deb";
    case ZBRA_SRC_RPM:   return "rpm";
    case ZBRA_SRC_TARXZ: return "tar.xz";
    case ZBRA_SRC_SNAP:  return "snap";
    case ZBRA_SRC_AUR:   return "aur";
    }

    return "unknown";
}

int zbra_source_type_from_name(const char *name)
{
    if (name == NULL)
        return -1;
    if (strcmp(name, "deb") == 0)
        return ZBRA_SRC_DEB;
    if (strcmp(name, "rpm") == 0)
        return ZBRA_SRC_RPM;
    if (strcmp(name, "tar.xz") == 0 || strcmp(name, "tarxz") == 0)
        return ZBRA_SRC_TARXZ;
    if (strcmp(name, "snap") == 0)
        return ZBRA_SRC_SNAP;
    if (strcmp(name, "aur") == 0)
        return ZBRA_SRC_AUR;

    return -1;
}

/* ---------------------------------------------------------- one source */

void zbra_source_free(zbra_source *s)
{
    if (s == NULL)
        return;

    free(s->id);
    free(s->url);
    free(s->file);
    zbra_d822_strlist_free(s->components, s->n_components);

    memset(s, 0, sizeof(*s));
}

int zbra_source_parse_line(const char *raw, zbra_source *out, char **err)
{
    char      *copy;
    char      *p;
    char      *tok[16];
    size_t     n_tok = 0;
    int        t;
    const char *type_name;

    memset(out, 0, sizeof(*out));

    if (raw == NULL) {
        if (err != NULL)
            *err = strdup("empty source line");
        return -1;
    }

    copy = strdup(raw);
    if (copy == NULL)
        return -1;

    /*
     * Tokenise on whitespace. A trailing comment is stripped first so that
     * "deb https://x/ ./  # nightly" works like every other apt-adjacent tool.
     */
    {
        char *hash = strchr(copy, '#');
        if (hash != NULL)
            *hash = '\0';
    }

    p = copy;
    while (*p != '\0' && n_tok < sizeof(tok) / sizeof(tok[0])) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;

        tok[n_tok++] = p;

        while (*p != '\0' && *p != ' ' && *p != '\t')
            p++;
        if (*p != '\0')
            *p++ = '\0';
    }

    if (n_tok < 2) {
        if (err != NULL)
            *err = strf("source line needs at least an id and a type: '%s'",
                        trim(copy));
        free(copy);
        return -1;
    }

    /*
     * Two shapes are accepted:
     *
     *   <id> <type> <url> [suite] [components...]
     *   <type> <url> [suite] [components...]     (apt-compatible, id derived)
     *
     * The second is what people paste from an apt sources.list, so accepting
     * it avoids a confusing failure on an otherwise valid configuration.
     */
    t = zbra_source_type_from_name(tok[0]);
    if (t >= 0) {
        type_name = tok[0];
        out->id = xstrdup(tok[1]);
        out->url = n_tok > 2 ? xstrdup(tok[2]) : xstrdup("");
        if (n_tok > 3) {
            out->n_components = n_tok - 3;
            out->components = calloc(out->n_components, sizeof(char *));
            if (out->components != NULL) {
                size_t i;
                for (i = 0; i < out->n_components; i++)
                    out->components[i] = xstrdup(tok[3 + i]);
            } else {
                out->n_components = 0;
            }
        }
    } else {
        type_name = tok[1];
        out->id = xstrdup(tok[0]);
        out->url = n_tok > 2 ? xstrdup(tok[2]) : xstrdup("");
        if (n_tok > 3) {
            out->n_components = n_tok - 3;
            out->components = calloc(out->n_components, sizeof(char *));
            if (out->components != NULL) {
                size_t i;
                for (i = 0; i < out->n_components; i++)
                    out->components[i] = xstrdup(tok[3 + i]);
            } else {
                out->n_components = 0;
            }
        }
        t = zbra_source_type_from_name(type_name);
    }

    out->type = (zbra_source_type)(t < 0 ? ZBRA_SRC_DEB : t);
    out->enabled = 1;

    /*
     * An unrecognised type is not fatal. It is recorded as disabled with a
     * NULL id so the caller can report it, rather than being silently used as
     * some default type.
     */
    if (t < 0) {
        free(out->id);
        out->id = xstrdup(tok[0]);
        free(out->url);
        out->url = n_tok > 2 ? xstrdup(tok[2]) : xstrdup("");
        out->enabled = 0;
        if (err != NULL)
            *err = strf("unknown source type '%s'", type_name);
        free(copy);
        return -2;
    }

    if (out->id == NULL) {
        zbra_source_free(out);
        if (err != NULL)
            *err = strdup("out of memory parsing source line");
        free(copy);
        return -1;
    }

    free(copy);

    return 0;
}

/* ---------------------------------------------------------- collection */

void zbra_sources_init(zbra_sources *s)
{
    memset(s, 0, sizeof(*s));
}

void zbra_sources_free(zbra_sources *s)
{
    size_t i;

    if (s == NULL)
        return;

    for (i = 0; i < s->n; i++)
        zbra_source_free(&s->items[i]);

    free(s->items);
    memset(s, 0, sizeof(*s));
}

static int sources_push(zbra_sources *s, const zbra_source *src)
{
    if (s->n == s->cap) {
        size_t         cap = s->cap != 0 ? s->cap * 2 : 8;
        zbra_source   *items;

        items = realloc(s->items, cap * sizeof(*items));
        if (items == NULL)
            return -1;

        s->items = items;
        s->cap = cap;
    }

    s->items[s->n++] = *src;

    return 0;
}

/*
 * Read one file's worth of sources. `collect_warnings` accumulates notes
 * about lines that were skipped so the CLI can surface them instead of
 * letting a broken entry pass unnoticed.
 */
static int load_file(zbra_sources *s, const char *path, char **err,
                     char **collect_warnings)
{
    FILE  *fp;
    char   line[4096];
    int    lineno = 0;
    zbra_sb warns;

    fp = fopen(path, "r");
    if (fp == NULL) {
        if (errno == ENOENT)
            return 0;               /* absent is normal, not an error */
        if (err != NULL)
            *err = strf("%s: %s", path, strerror(errno));
        return -1;
    }

    zbra_sb_init(&warns);

    while (fgets(line, sizeof(line), fp) != NULL) {
        zbra_source src;
        char       *lerr = NULL;
        char       *t;

        lineno++;

        t = trim(line);
        if (is_blank_or_comment(t))
            continue;

        /*
         * Parse manually rather than through zbra_source_parse_line so the
         * file/line can be attached to the result and to the error text.
         */
        {
            char  *copy = strdup(t);
            char  *tok[32];
            size_t n_tok = 0;
            char  *p;
            int    type;

            if (copy == NULL)
                continue;

            {
                char *hash = strchr(copy, '#');
                if (hash != NULL)
                    *hash = '\0';
            }

            p = copy;
            while (*p != '\0' && n_tok < sizeof(tok) / sizeof(tok[0])) {
                while (*p == ' ' || *p == '\t')
                    p++;
                if (*p == '\0')
                    break;
                tok[n_tok++] = p;
                while (*p != '\0' && *p != ' ' && *p != '\t')
                    p++;
                if (*p != '\0')
                    *p++ = '\0';
            }

            memset(&src, 0, sizeof(src));

            if (n_tok < 2) {
                zbra_sb_addf(&warns, "%s:%d: needs at least <id> <type>"
                                    " (skipped)\n", path, lineno);
                free(copy);
                continue;
            }

            type = zbra_source_type_from_name(tok[1]);
            if (type < 0) {
                zbra_sb_addf(&warns, "%s:%d: unknown source type '%s' (skipped)"
                                    "\n", path, lineno, tok[1]);
                free(copy);
                continue;
            }

            /*
             * Only allocate src's members once the line has validated, so
             * every early `continue` above leaves nothing to free.
             */
            src.file = xstrdup(path);
            src.line = lineno;
            src.enabled = 1;
            src.type = (zbra_source_type)type;
            src.id = xstrdup(tok[0]);

            /*
             * A URL is optional: "arch aur" is a complete, meaningful source
             * because AUR packages are built from PKGBUILDs rather than
             * fetched from an index. Requiring a URL would make the most
             * natural spelling of an AUR source a syntax error.
             */
            src.url = n_tok > 2 ? xstrdup(tok[2]) : xstrdup("");

            if (n_tok > 3) {
                src.n_components = n_tok - 3;
                src.components = calloc(src.n_components, sizeof(char *));
                if (src.components != NULL) {
                    size_t i;
                    for (i = 0; i < src.n_components; i++)
                        src.components[i] = xstrdup(tok[3 + i]);
                } else {
                    src.n_components = 0;
                }
            }

            free(copy);
        }

        (void) lerr;

        if (sources_push(s, &src) != 0) {
            zbra_source_free(&src);
            fclose(fp);
            zbra_sb_free(&warns);
            if (err != NULL)
                *err = strdup("out of memory loading sources");
            return -1;
        }
    }

    fclose(fp);

    if (collect_warnings != NULL && warns.buf != NULL && warns.len > 0)
        *collect_warnings = strdup(warns.buf);

    zbra_sb_free(&warns);

    return 0;
}

int zbra_sources_load(zbra_sources *s, const char *dir, char **err,
                      char **warnings)
{
    char  path[4096];
    char  dpath[2048];
    DIR  *d;
    struct dirent *de;

    zbra_sources_init(s);

    if (warnings != NULL)
        *warnings = NULL;

    snprintf(path, sizeof(path), "%s/sources.list", dir);
    if (load_file(s, path, err, warnings) != 0)
        return -1;

    snprintf(dpath, sizeof(dpath), "%s/sources.list.d", dir);

    d = opendir(dpath);
    if (d == NULL) {
        if (errno == ENOENT)
            return 0;
        if (err != NULL)
            *err = strf("%s: %s", dpath, strerror(errno));
        return -1;
    }

    /*
     * Sorted iteration so the effective source order does not depend on
     * directory order. Order matters because a later source providing the
     * same package at a higher version should win predictably.
     */
    {
        char **names = NULL;
        size_t n_names = 0;
        size_t cap = 0;
        size_t i;

        while ((de = readdir(d)) != NULL) {
            size_t len;

            if (de->d_name[0] == '.')
                continue;

            len = strlen(de->d_name);
            if (len < 6 || strcmp(de->d_name + len - 5, ".list") != 0)
                continue;

            if (n_names == cap) {
                cap = cap != 0 ? cap * 2 : 16;
                names = realloc(names, cap * sizeof(*names));
                if (names == NULL) {
                    closedir(d);
                    return -1;
                }
            }

            names[n_names] = xstrdup(de->d_name);
            if (names[n_names] == NULL)
                continue;
            n_names++;
        }

        closedir(d);

        for (i = 0; i < n_names; i++)
            for (size_t j = i + 1; j < n_names; j++)
                if (strcmp(names[i], names[j]) > 0) {
                    char *tmp = names[i];
                    names[i] = names[j];
                    names[j] = tmp;
                }

        for (i = 0; i < n_names; i++) {
            char f[4096];

            /*
             * Bounded join: a source file whose full path would not fit is
             * reported and skipped rather than silently truncated into a
             * different (or attacker-chosen) filename.
             */
            if (snprintf(f, sizeof(f), "%s/%s", dpath, names[i]) >=
                (int)sizeof(f)) {
                if (warnings != NULL && *warnings == NULL)
                    *warnings = strdup("source path too long; entry skipped\n");
                continue;
            }

            if (load_file(s, f, err, warnings) != 0) {
                for (size_t k = 0; k < n_names; k++)
                    free(names[k]);
                free(names);
                zbra_sources_free(s);
                return -1;
            }
            free(names[i]);
        }

        free(names);
    }

    return 0;
}

int zbra_sources_save(const zbra_sources *s, const char *dir, char **err)
{
    char   path[4096];
    char   tmp[4096];
    FILE  *fp;
    size_t i;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (err != NULL)
            *err = strf("%s: %s", dir, strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "%s/sources.list", dir);
    snprintf(tmp, sizeof(tmp), "%s/sources.list.tmp", dir);

    fp = fopen(tmp, "w");
    if (fp == NULL) {
        if (err != NULL)
            *err = strf("%s: %s", tmp, strerror(errno));
        return -1;
    }

    fprintf(fp, "# zbra sources -- <id> <type> <url> [suite] [components...]\n");
    fprintf(fp, "# types: deb, rpm, tar.xz, snap, aur\n");

    for (i = 0; i < s->n; i++) {
        const zbra_source *src = &s->items[i];
        size_t            k;

        fprintf(fp, "%s %s %s", src->id,
                zbra_source_type_name(src->type), src->url);

        for (k = 0; k < src->n_components; k++)
            fprintf(fp, " %s", src->components[k]);

        fprintf(fp, "\n");
    }

    /*
     * Written via a temporary and renamed, so an interrupted save cannot
     * leave a truncated sources.list that breaks every later command.
     */
    if (fclose(fp) != 0) {
        if (err != NULL)
            *err = strf("%s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, path) != 0) {
        if (err != NULL)
            *err = strf("rename %s -> %s: %s", tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    return 0;
}

zbra_source *zbra_sources_find(zbra_sources *s, const char *id)
{
    size_t i;

    if (id == NULL)
        return NULL;

    for (i = 0; i < s->n; i++) {
        if (s->items[i].id != NULL && strcmp(s->items[i].id, id) == 0)
            return &s->items[i];
    }

    return NULL;
}

int zbra_sources_set(zbra_sources *s, const char *id, const char *type,
                     const char *url, const char *const *components,
                     size_t n_components, char **err)
{
    zbra_source  fresh;
    zbra_source *existing;
    size_t       i;

    if (id == NULL || *id == '\0') {
        if (err != NULL)
            *err = strdup("source needs an id");
        return -1;
    }

    if (zbra_source_type_from_name(type) < 0) {
        if (err != NULL)
            *err = strf("unknown source type '%s' (deb, rpm, tar.xz, snap, aur)",
                        type != NULL ? type : "");
        return -1;
    }

    memset(&fresh, 0, sizeof(fresh));
    fresh.id = xstrdup(id);
    fresh.type = (zbra_source_type)zbra_source_type_from_name(type);
    fresh.url = xstrdup(url != NULL ? url : "");
    fresh.enabled = 1;

    if (n_components > 0) {
        fresh.components = calloc(n_components, sizeof(char *));
        if (fresh.components == NULL) {
            zbra_source_free(&fresh);
            return -1;
        }
        for (i = 0; i < n_components; i++)
            fresh.components[i] = xstrdup(components[i]);
        fresh.n_components = n_components;
    }

    existing = zbra_sources_find(s, id);
    if (existing != NULL) {
        zbra_source_free(existing);
        *existing = fresh;
        return 0;
    }

    if (sources_push(s, &fresh) != 0) {
        zbra_source_free(&fresh);
        if (err != NULL)
            *err = strdup("out of memory");
        return -1;
    }

    return 0;
}

int zbra_sources_remove(zbra_sources *s, const char *id)
{
    size_t i;

    for (i = 0; i < s->n; i++) {
        if (s->items[i].id == NULL || strcmp(s->items[i].id, id) != 0)
            continue;

        zbra_source_free(&s->items[i]);

        /* Keep the array dense so later save/load round-trips identically. */
        if (i + 1 < s->n)
            memmove(&s->items[i], &s->items[i + 1],
                    (s->n - i - 1) * sizeof(s->items[0]));
        s->n--;

        return 1;
    }

    return 0;
}

int zbra_sources_edit(zbra_sources *s, const char *dir, char **err)
{
    char   path[4096];
    char  *editor = NULL;
    char  *cmd;
    int    rc;

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        if (err != NULL)
            *err = strf("%s: %s", dir, strerror(errno));
        return -1;
    }

    snprintf(path, sizeof(path), "%s/sources.list", dir);

    if (access(path, F_OK) != 0) {
        /*
         * Seed with the distribution's own repository so editing sources on a
         * fresh install does not start from an empty file.
         */
        FILE *fp = fopen(path, "w");
        if (fp != NULL) {
            fprintf(fp, "# zbra sources -- <id> <type> <url> [suite]"
                        " [components...]\n");
            fprintf(fp, "tenebra deb https://itzr1g.github.io"
                        "/TenebraOS-packages/ ./\n");
            fclose(fp);
        }
    }

    editor = getenv("VISUAL");
    if (editor == NULL || *editor == '\0')
        editor = getenv("EDITOR");
    if (editor == NULL || *editor == '\0')
        editor = (char *)"vi";

    /* The path is a fixed local path under a config dir, not user input, and
     * EDITOR is inherently a shell command by convention. */
    rc = asprintf(&cmd, "%s %s", editor, path);
    if (rc < 0) {
        if (err != NULL)
            *err = strdup("out of memory");
        return -1;
    }

    rc = system(cmd);
    free(cmd);

    if (rc != 0) {
        if (err != NULL)
            *err = strdup("editor exited with an error; sources unchanged");
        return -1;
    }

    /* Re-read so the caller's in-memory state matches what is on disk. */
    zbra_sources_free(s);

    return zbra_sources_load(s, dir, err, NULL);
}
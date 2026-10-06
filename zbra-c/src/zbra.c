/*
 * zbra -- the TenebraOS package manager.
 *
 * This file is the wiring: it turns the modules' APIs into commands and owns
 * the flows between them (load sources, update the index, resolve, stage,
 * commit, record). Each module keeps its own invariants, and this file is
 * where the order of operations and the user-facing wording live.
 *
 * Commands, in the order they run their work:
 *
 *   search            query the local index; never touches the network
 *   show              one package's metadata and files
 *   list              what is installed, per registry
 *   update            refresh every configured source
 *   install           resolve, stage, commit, record
 *   remove            unlink files, then forget the entry
 *   verify            report conflicts, broken dependencies, stray files
 *   sources           list, add, remove, edit configured sources
 *   edit-sources      open the source list in $EDITOR
 *
 * A design note that explains the shape of a lot of this file: `update` is
 * separate from every other command. Installing from a stale index is how a
 * user gets a version they did not ask for, and making the refresh explicit
 * means the state they resolve against is the state they can see. Search and
 * show are offline for the same reason.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "commit.h"
#include "db.h"
#include "deb822.h"
#include "deps.h"
#include "index.h"
#include "proc.h"
#include "sha256.h"
#include "sources.h"
#include "version.h"
#include "vercmp.h"

#define PROGNAME "zbra"

static void usage(FILE *out);

/* ------------------------------------------------------------- plumbing */

static void die(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, PROGNAME ": ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    exit(1);
}

/* Release a reason string from a module and exit with it. */
static void die_err(char *err)
{
    die("%s", err != NULL ? err : "unknown error");
    /* die() does not return, so err is unreachable; kept for clarity. */
}

static char *sconfig_dir(void)
{
    const char *env = getenv("ZBRA_CONFIG_DIR");

    return strdup(env != NULL && *env != '\0' ? env : "/etc/zbra");
}

static const char *cache_dir(void)
{
    const char *env = getenv("ZBRA_CACHE_DIR");

    return env != NULL && *env != '\0' ? env : "/var/cache/zbra";
}

/* --------------------------------------------------------- installed ver */

/* The resolver's view of what is already on the system. */
static const char *db_installed_version(void *ud, const char *name)
{
    zbra_db     *db = ud;
    zbra_entry   e;
    static char  buf[256];

    if (zbra_db_get(db, name, &e) != 1)
        return NULL;

    snprintf(buf, sizeof(buf), "%s", e.version != NULL ? e.version : "");
    zbra_entry_free(&e);

    return buf;
}

/* ------------------------------------------------------------- searching */

static void cmd_search(const char *needle)
{
    zbra_index    idx;
    zbra_package *hits[64];
    char         *err = NULL;
    char         *cfg;
    char         *path;
    size_t        n;
    size_t        i;

    zbra_index_init(&idx);

    cfg = sconfig_dir();
    path = malloc(strlen(cache_dir()) + 32);
    if (path == NULL)
        die("out of memory");
    sprintf(path, "%s/index.json", cache_dir());

    /*
     * Searching a missing index is a normal state on a fresh install, not an
     * error: the fix is `zbra update`, and saying so beats a bare ENOENT.
     */
    if (access(path, R_OK) != 0) {
        printf("No index yet. Run \"%s update\" first.\n", PROGNAME);
        free(cfg);
        free(path);
        return;
    }

    if (zbra_index_read_json(&idx, path, "local", "file", &err) != 0)
        die_err(err);

    n = zbra_index_search(&idx, needle, hits, 64);

    if (n == 0) {
        printf("Nothing matches \"%s\".\n", needle);
    } else {
        for (i = 0; i < n; i++)
            printf("%-30s %-16s %s\n", hits[i]->name,
                   hits[i]->version != NULL ? hits[i]->version : "-",
                   hits[i]->description != NULL ? hits[i]->description : "");
    }

    zbra_index_free(&idx);
    free(cfg);
    free(path);
}

static void cmd_show(const char *name)
{
    zbra_db    db;
    zbra_entry e;
    size_t     i;

    if (zbra_db_open(&db, zbra_db_default_root()) != 0)
        die("cannot open the database: %s", strerror(errno));

    if (zbra_db_get(&db, name, &e) != 1) {
        printf("%s is not installed.\n", name);
        zbra_db_close(&db);
        return;
    }

    printf("Name:      %s\n", e.name);
    printf("Version:   %s\n", e.version != NULL ? e.version : "-");
    printf("Format:    %s\n", e.format != NULL ? e.format : "-");
    printf("Source:    %s\n", e.source != NULL ? e.source : "-");
    if (e.arch != NULL)
        printf("Arch:      %s\n", e.arch);
    printf("Kind:      %s\n",
           e.kind == ZBRA_KIND_PACKAGE
               ? "explicitly installed"
               : (e.kind == ZBRA_KIND_ISOLATED ? "isolated"
                                               : "installed as a dependency"));

    if (e.n_depends > 0) {
        printf("Depends:\n");
        for (i = 0; i < e.n_depends; i++)
            printf("  %s\n", e.depends[i]);
    }

    if (e.n_files > 0) {
        printf("Files:\n");
        for (i = 0; i < e.n_files; i++)
            printf("  %s\n", e.files[i]);
    }

    zbra_entry_free(&e);
    zbra_db_close(&db);
}

static void print_entries(char **names, size_t n, const char *title)
{
    size_t i;

    if (n == 0)
        return;

    printf("%s:\n", title);
    for (i = 0; names[i] != NULL; i++)
        printf("  %s\n", names[i]);
    putchar('\n');
}

static void cmd_list(void)
{
    zbra_db  db;
    char   **names;
    size_t   n;

    if (zbra_db_open(&db, zbra_db_default_root()) != 0)
        die("cannot open the database: %s", strerror(errno));

    names = zbra_db_list(&db, ZBRA_KIND_PACKAGE, &n);
    print_entries(names, n, "Packages");
    zbra_strlist_free_owned(names);

    names = zbra_db_list(&db, ZBRA_KIND_DEPENDENCY, &n);
    print_entries(names, n, "Dependencies");
    zbra_strlist_free_owned(names);

    names = zbra_db_list(&db, ZBRA_KIND_ISOLATED, &n);
    print_entries(names, n, "Isolated");
    zbra_strlist_free_owned(names);

    zbra_db_close(&db);
}

/* --------------------------------------------------------------- sources */

/*
 * Load the configuration and report anything skipped along the way.
 *
 * zbra_sources_load deliberately keeps going past an unrecognised line
 * rather than refusing to start, so the warnings are the only way the user
 * finds out a source is being ignored.
 */
static int load_sources(zbra_sources *src, const char *dir)
{
    char  *err = NULL;
    char  *warnings = NULL;

    zbra_sources_init(src);

    if (zbra_sources_load(src, dir, &err, &warnings) != 0)
        die_err(err);

    if (warnings != NULL) {
        fprintf(stderr, PROGNAME ": %s\n", warnings);
        free(warnings);
    }

    return 0;
}

static void save_sources(zbra_sources *src, const char *dir)
{
    char *err = NULL;

    if (zbra_sources_save(src, dir, &err) != 0)
        die_err(err);
}

static void cmd_sources(int argc, char **argv)
{
    zbra_sources src;
    char        *cfg;
    char        *err = NULL;
    size_t       i;

    cfg = sconfig_dir();

    if (argc == 0) {
        load_sources(&src, cfg);

        if (src.n == 0) {
            printf("No sources configured. Add one with \"%s sources add\".\n",
                   PROGNAME);
        } else {
            printf("%-20s %-10s %s\n", "ID", "TYPE", "URL");
            for (i = 0; i < src.n; i++) {
                const char *url = src.items[i].url != NULL ? src.items[i].url
                                                            : "(local)";
                printf("%-20s %-10s %s\n", src.items[i].id,
                       zbra_source_type_name(src.items[i].type), url);
            }
        }

        zbra_sources_free(&src);
        free(cfg);
        return;
    }

    if (strcmp(argv[0], "add") == 0) {
        if (argc < 4)
            die("usage: %s sources add <id> <type> <url>", PROGNAME);

        load_sources(&src, cfg);

        if (zbra_sources_set(&src, argv[1], argv[2], argv[3], NULL, 0, &err)
            != 0)
            die_err(err);

        save_sources(&src, cfg);

        printf("Added source %s (%s).\n", argv[1], argv[2]);
        zbra_sources_free(&src);
        free(cfg);
        return;
    }

    if (strcmp(argv[0], "remove") == 0) {
        if (argc < 2)
            die("usage: %s sources remove <id>", PROGNAME);

        load_sources(&src, cfg);

        /* Returns 1 when it removed something, 0 when the id was unknown. */
        if (zbra_sources_remove(&src, argv[1]) == 0)
            die("no source named \"%s\"", argv[1]);

        save_sources(&src, cfg);

        printf("Removed source %s.\n", argv[1]);
        zbra_sources_free(&src);
        free(cfg);
        return;
    }

    die("unknown sources subcommand \"%s\"", argv[0]);
}

static void cmd_edit_sources(void)
{
    zbra_sources src;
    char        *err = NULL;
    char        *cfg;

    cfg = sconfig_dir();

    load_sources(&src, cfg);

    if (zbra_sources_edit(&src, cfg, &err) != 0)
        die_err(err);

    zbra_sources_free(&src);
    free(cfg);
}

/* ---------------------------------------------------------------- update */

static void cmd_update(void)
{
    zbra_sources src;
    zbra_index   idx;
    char        *err = NULL;
    char        *cfg;
    size_t       i;

    cfg = sconfig_dir();

    load_sources(&src, cfg);

    if (src.n == 0) {
        printf("No sources configured; nothing to update.\n");
        free(cfg);
        return;
    }

    zbra_index_init(&idx);

    for (i = 0; i < src.n; i++) {
        const zbra_source *s = &src.items[i];

        if (s->url == NULL || *s->url == '\0') {
            printf("Skipping %s: it has no URL.\n", s->id);
            continue;
        }

        printf("Updating %s (%s)...\n", s->id, s->url);

        /*
         * Only the local "file" transport is wired up here. Fetching over the
         * network needs checksum-verified downloads with a cache, and
         * pretending otherwise would mean resolving against an index that may
         * not match what the server sent.
         */
        if (strncmp(s->url, "file://", 7) == 0 || s->url[0] == '/') {
            const char *path = strncmp(s->url, "file://", 7) == 0
                                   ? s->url + 7
                                   : s->url;
            struct stat st;
            zbra_index  local;

            zbra_index_init(&local);

            if (stat(path, &st) != 0) {
                printf("  failed: %s is not there\n", path);
                zbra_index_free(&local);
                continue;
            }

            /*
             * A directory of packages is scanned; anything else is treated as
             * an index file and parsed. A source that publishes a real index
             * should point at it, because a file name cannot carry
             * dependencies or checksums the way an index can -- which is
             * exactly what scanning is good enough at and no more than.
             */
            if (S_ISDIR(st.st_mode)) {
                if (zbra_index_scan_directory(&local, path, s->id, s->url,
                                              &err) != 0) {
                    printf("  failed: %s\n", err != NULL ? err : "scan failed");
                    free(err);
                    err = NULL;
                    zbra_index_free(&local);
                    continue;
                }
            } else if (zbra_index_read_auto(&local, path, s->id, s->url, &err)
                       != 0) {
                printf("  failed: %s\n", err != NULL ? err : "read failed");
                free(err);
                err = NULL;
                zbra_index_free(&local);
                continue;
            }

            if (zbra_index_merge(&idx, &local) != 0)
                die("out of memory merging the index");

            printf("  %zu packages\n", local.n);
            zbra_index_free(&local);
        } else {
            printf("  skipped: %s URLs are not supported yet.\n",
                   zbra_source_type_name(s->type));
        }
    }

    if (zbra_mkdir_p(cache_dir(), 0755) != 0)
        die("cannot create %s: %s", cache_dir(), strerror(errno));

    {
        char  path[4096];
        FILE *fp;

        snprintf(path, sizeof(path), "%s/index.json", cache_dir());
        if (zbra_index_write_json(&idx, path, &err) != 0)
            die_err(err);

        fp = fopen(path, "r");
        if (fp != NULL) {
            fclose(fp);
        }
    }

    /*
     * Warnings are reported rather than swallowed: a source that served
     * records zbra had to drop is the difference between "you have 400
     * packages" and "you have 396 and something is wrong".
     */
    for (i = 0; i < idx.n_warnings; i++)
        fprintf(stderr, PROGNAME ": warning: %s\n", idx.warnings[i]);

    printf("Index holds %zu packages", idx.n);
    if (idx.n_warnings > 0)
        printf(" (%zu dropped)", idx.n_warnings);
    putchar('\n');

    zbra_index_free(&idx);
    zbra_sources_free(&src);
    free(cfg);
}

/*
 * The root a package is unpacked into.
 *
 * Kept in one place because install and remove both have to agree: a file
 * recorded as "/usr/bin/foo" is meaningless if remove resolves it somewhere
 * else, and that mismatch shows up as files that survive their own removal.
 */
static const char *install_root(void)
{
    const char *r = getenv("ZBRA_INSTALL_ROOT");

    return r != NULL && r[0] != 0 ? r : "/";
}

/*
 * Check the cached payload against the index's checksum.
 *
 * Returns 0 when the file is acceptable, or -1 with a reason in *err. An index
 * entry with no checksum, or one in a form zbra cannot compute, is a refusal
 * rather than a pass: the whole point of the field is that an unverifiable
 * payload is not an installed one, and treating it as verified would make the
 * field optional in practice.
 */
static int verify_payload(const zbra_package *pkg, const char *path, char **err)
{
    const char *sep;
    int         rc;

    if (pkg->checksum == NULL || pkg->checksum[0] == 0) {
        if (err != NULL)
            *err = strdup("the index gives no checksum, so this payload "
                          "cannot be trusted");
        return -1;
    }

    rc = zbra_sha256_check(path, pkg->checksum);

    if (rc == 0)
        return 0;

    sep = strchr(pkg->checksum, ':');

    if (rc == -2) {
        if (err != NULL) {
            char *why;

            asprintf(&why, "the checksum \"%s\" is in a format zbra cannot "
                           "verify", pkg->checksum);
            *err = why;
        }

        return -1;
    }

    if (err != NULL) {
        char *why;

        asprintf(&why, "%s does not match the index: expected %s", path,
                 sep != NULL ? sep + 1 : pkg->checksum);
        *err = why;
    }

    return -1;
}

/*
 * Put a package payload in the cache, returning the path to use.
 *
 * A cached payload is used as-is. Otherwise the source is consulted, which
 * today means copying from a local repository directory. There is no network
 * fetch here on purpose: an installer that downloads bytes without verifying
 * them would make "installed" a claim about whatever the connection happened
 * to return, and the index checksum is the only thing standing between a
 * mirror and the root filesystem.
 */
static int fetch_payload(const zbra_package *pkg, const char *file,
                         char *out, size_t out_len, char **err)
{
    char    src[4096];
    char    dst[4096];
    char    buf[65536];
    int     in;
    int     out_fd;
    ssize_t n;
    const char *base;
    char    why[4200];

    if (file == NULL || file[0] == 0) {
        if (err != NULL)
            *err = strdup("the index does not say which file this package is");
        return -1;
    }

    if (strchr(file, '/') != NULL) {
        if (err != NULL)
            *err = strdup("the index lists a file name with a path in it");
        return -1;
    }

    snprintf(dst, sizeof(dst), "%s/%s", cache_dir(), file);
    snprintf(out, out_len, "%s", dst);

    /*
     * A file already in the cache is not automatically the file the index asks
     * for. The cache outlives an index update, so one name can be re-pointed at
     * different bytes, and a truncated or interrupted copy leaves something
     * that reads fine. This is the cheapest place to notice, before the bytes
     * are staged.
     */
    if (access(dst, R_OK) == 0)
        return verify_payload(pkg, dst, err);

    if (pkg->source_url == NULL ||
        (strncmp(pkg->source_url, "file://", 7) != 0 &&
         pkg->source_url[0] != '/')) {
        if (err != NULL)
            *err = strdup("downloading from the network is not implemented "
                          "yet; put the package in the cache by hand");
        return -1;
    }

    base = strncmp(pkg->source_url, "file://", 7) == 0 ? pkg->source_url + 7
                                                       : pkg->source_url;
    snprintf(src, sizeof(src), "%s/%s", base, file);

    if (zbra_mkdir_p(cache_dir(), 0755) != 0) {
        if (err != NULL)
            *err = strdup("cannot create the cache directory");
        return -1;
    }

    in = open(src, O_RDONLY);
    if (in < 0) {
        if (err != NULL) {
            snprintf(why, sizeof(why), "cannot read %s: %s", src,
                     strerror(errno));
            *err = strdup(why);
        }
        return -1;
    }

    out_fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out_fd < 0) {
        close(in);
        if (err != NULL) {
            snprintf(why, sizeof(why), "cannot write %s: %s", dst,
                     strerror(errno));
            *err = strdup(why);
        }
        return -1;
    }

    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;

        while (off < n) {
            ssize_t w = write(out_fd, buf + off, (size_t)(n - off));

            if (w <= 0) {
                close(in);
                close(out_fd);
                unlink(dst);
                if (err != NULL)
                    *err = strdup("the cache copy failed part way through");
                return -1;
            }
            off += w;
        }
    }

    close(in);
    close(out_fd);

    if (n < 0) {
        unlink(dst);
        if (err != NULL)
            *err = strdup("the cache copy failed part way through");
        return -1;
    }

    /*
     * The copy is verified before it is reported as available, so a file that
     * does not match is removed. Leaving it would be worse than useless: the
     * next call would find it in the cache, skip the copy, and take the same
     * wrong answer from the branch above.
     */
    if (verify_payload(pkg, dst, err) != 0) {
        unlink(dst);
        return -1;
    }

    return 0;
}

/*
 * Record, for every installed package, which installed packages need it.
 *
 * This runs once after the whole plan has been installed rather than per
 * package, and the distinction is not tidiness. Registering "A needs B" while
 * A is being installed requires B to already be in the database, which is only
 * true if the resolver happened to order the plan dependencies-first. When it
 * did not, the link was silently dropped and `remove B` would happily delete a
 * library something still needed. Doing it as a second pass over what is
 * actually on disk makes the answer independent of plan order.
 *
 * The requirement string is parsed to get the name. Passing the raw text to the
 * lookup is the bug this replaces: "libc6 (>= 2.38)" is not a package name, so
 * the lookup never matched and no dependency was ever protected.
 */
static void record_dependents(zbra_db *db)
{
    char   **names = NULL;
    size_t   n = 0;
    size_t   i;
    size_t   r;

    names = zbra_db_list(db, ZBRA_KIND_ANY, &n);

    if (names == NULL)
        return;

    for (i = 0; names[i] != NULL; i++) {
        zbra_entry e;

        if (zbra_db_get(db, names[i], &e) != 1)
            continue;

        for (r = 0; r < e.n_depends; r++) {
            zbra_dep_full d;
            zbra_entry    dep;

            if (e.depends[r] == NULL)
                continue;

            if (zbra_dep_parse(e.depends[r], &d) != 0) {
                fprintf(stderr, PROGNAME ": warning: cannot record what %s "
                                        "depends on: \"%s\" is not a "
                                        "requirement\n",
                        e.name, e.depends[r]);
                continue;
            }

            /*
             * Only a name that is genuinely installed becomes a reverse
             * dependency. An optional or unmet alternative is recorded in the
             * entry's requirements, where `verify` will report it, but it must
             * not block removal of a package that is not there.
             */
            if (zbra_db_get(db, d.name, &dep) == 1) {
                if (zbra_db_add_dependent(db, dep.name, e.name) != 0)
                    fprintf(stderr, PROGNAME ": warning: cannot record that "
                                            "%s needs %s: %s\n",
                            e.name, dep.name, strerror(errno));
                zbra_entry_free(&dep);
            }

            zbra_dep_free(&d);
        }

        zbra_entry_free(&e);
    }

    zbra_strlist_free_owned(names);
}

/* --------------------------------------------------------------- install */

static void cmd_install(const char *name)
{
    zbra_db      db;
    zbra_index   idx;
    zbra_plan    plan;
    zbra_resolver res;
    char        *err = NULL;
    char         path[4096];
    size_t       i;
    size_t       k;
    size_t       skipped = 0;
    int          rc;

    if (zbra_db_open(&db, zbra_db_default_root()) != 0)
        die("cannot open the database: %s", strerror(errno));

    snprintf(path, sizeof(path), "%s/index.json", cache_dir());

    zbra_index_init(&idx);
    if (zbra_index_read_json(&idx, path, "local", "file", &err) != 0)
        die("cannot read the index: %s. Run \"%s update\" first.",
            err != NULL ? err : "unknown error", PROGNAME);

    res.find_candidate    = zbra_index_find_candidate;
    res.installed_version = db_installed_version;
    res.ud                = &idx;
    res.ud_installed      = &db;

    zbra_plan_init(&plan);

    if (zbra_deps_resolve(&res, name, &plan, &err) != 0) {
        fprintf(stderr, PROGNAME ": cannot install %s: %s\n", name,
                err != NULL ? err : "unresolved");
        zbra_plan_free(&plan);
        zbra_index_free(&idx);
        zbra_db_close(&db);
        exit(1);
    }

    printf("Plan (%zu package%s):\n", plan.n, plan.n == 1 ? "" : "s");
    for (i = 0; i < plan.n; i++)
        printf("  %s %s  [%s]\n", plan.items[i].name, plan.items[i].version,
               plan.items[i].format != NULL ? plan.items[i].format : "?");

    for (i = 0; i < plan.n; i++) {
        zbra_candidate *cand = &plan.items[i];
        zbra_package    pkg;
        zbra_commit    *commit = NULL;
        char           *why = NULL;
        char          **reqs = NULL;
        size_t          n_reqs = 0;

        if (cand->name == NULL || cand->version == NULL)
            continue;

        memset(&pkg, 0, sizeof(pkg));

        /*
         * The candidate carries no file name, so the full record is looked up
         * for it. Reading the index twice is cheaper than guessing, and a
         * guess here silently installs the wrong bytes.
         */
        if (zbra_index_find_copy(&idx, cand->name, &pkg) != 1)
            continue;

        if (fetch_payload(&pkg, pkg.filename, path, sizeof(path), &err) != 0) {
            skipped++;
            printf("Not installed: %s: %s\n", cand->name,
                   err != NULL ? err : "the package is not available");
            free(err);
            err = NULL;
            zbra_package_free(&pkg);
            continue;
        }

        if (!zbra_backend_claims(cand->format, cand->format, path)) {
            skipped++;
            printf("Not installed: %s: no backend handles %s.\n", cand->name,
                   cand->format != NULL ? cand->format : "this format");
            zbra_package_free(&pkg);
            continue;
        }

        if (zbra_backend_available(cand->format, &why) == 0) {
            skipped++;
            printf("Not installed: %s: %s\n", cand->name,
                   why != NULL ? why : "the backend is unavailable");
            free(why);
            zbra_package_free(&pkg);
            continue;
        }

        if (zbra_commit_open(&commit, zbra_db_default_root(), &err) != 0)
            die_err(err);

        if (zbra_backend_stage(cand->format, commit, path, &pkg, &err) != 0) {
            skipped++;
            fprintf(stderr, PROGNAME ": %s: %s\n", cand->name,
                    err != NULL ? err : "staging failed");
            free(err);
            err = NULL;
            zbra_commit_abort(commit);
            continue;
        }

        /*
         * Which files the package owns is captured before apply consumes the
         * commit. Without this list the entry could not be removed or
         * verified, so it is read first rather than reconstructed afterwards.
         */
        {
            size_t  owned = zbra_commit_count(commit);
            char  **files = owned > 0 ? calloc(owned, sizeof(*files)) : NULL;

            if (owned > 0 && files == NULL)
                die("out of memory recording %s", cand->name);

            for (k = 0; k < owned; k++)
                files[k] = strdup(zbra_commit_dest(commit, k));

            /*
             * The requirements are recorded too, not just the files.
             *
             * Without them there is nothing for `verify` to check against: it
             * would have to re-read the live index to learn what the package
             * depends on, which asks the repository what it wants today rather
             * than what this installation actually required. A dependency that
             * has since been dropped from an index would silently stop being
             * checked.
             */
            n_reqs = 0;
            for (k = 0; k < cand->n_depends; k++)
                if (cand->depends[k] != NULL && cand->depends[k][0] != 0)
                    n_reqs++;

            reqs = n_reqs > 0 ? calloc(n_reqs, sizeof(*reqs)) : NULL;
            if (n_reqs > 0 && reqs == NULL)
                die("out of memory recording %s", cand->name);

            for (k = 0, i = 0; k < cand->n_depends; k++)
                if (cand->depends[k] != NULL && cand->depends[k][0] != 0)
                    reqs[i++] = strdup(cand->depends[k]);

            rc = zbra_commit_apply(&commit, install_root(), &err);
            if (rc != 0) {
                skipped++;
                fprintf(stderr, PROGNAME ": %s: %s\n", cand->name,
                        err != NULL ? err : "install failed");
                free(err);
                err = NULL;
                for (k = 0; k < owned; k++)
                    free(files[k]);
                free(files);
                for (k = 0; k < n_reqs; k++)
                    free(reqs[k]);
                free(reqs);
                zbra_package_free(&pkg);
                continue;
            }

            /* Record it only once the files are really there. */
            {
                zbra_entry e;

                memset(&e, 0, sizeof(e));
                e.name         = cand->name;
                e.version      = cand->version;
                e.format       = cand->format;
                e.source       = cand->source;
                e.kind         = strcmp(cand->name, name) == 0
                                    ? ZBRA_KIND_PACKAGE
                                    : ZBRA_KIND_DEPENDENCY;
                e.files        = files;
                e.n_files      = owned;
                e.depends      = reqs;
                e.n_depends    = n_reqs;
                e.install_root = (char *)install_root();

                if (zbra_db_put(&db, &e) != 0)
                    fprintf(stderr, PROGNAME ": warning: %s installed but "
                                            "could not be recorded: %s\n",
                            cand->name, strerror(errno));

                /* zbra_db_put copies the entry, so these arrays are ours. */
                for (k = 0; k < owned; k++)
                    free(files[k]);
                free(files);
                for (k = 0; k < n_reqs; k++)
                    free(reqs[k]);
                free(reqs);
            }
        }

        zbra_package_free(&pkg);
        printf("Installed %s %s\n", cand->name, cand->version);
    }

    /*
     * A plan that did not fully install is a failure, not a partial success.
     *
     * Reporting success after skipping a package leaves the worst possible
     * outcome: the message says it was skipped, the exit status says everything
     * is fine, and a build script or provisioning step that checks only the
     * status carries on using a package that is not there. Anything that
     * reached this point and did not install has to show up in the exit code.
     */
    if (skipped > 0)
        fprintf(stderr, PROGNAME ": %zu of %zu package%s in the plan "
                                "could not be installed\n",
                skipped, plan.n, plan.n == 1 ? "" : "s");

    record_dependents(&db);
    zbra_plan_free(&plan);
    zbra_index_free(&idx);
    zbra_db_close(&db);

    exit(skipped > 0 ? 1 : 0);
}

/* ---------------------------------------------------------------- remove */

static void cmd_remove(const char *name)
{
    zbra_db     db;
    zbra_entry  e;
    char       *owner;
    size_t      i;
    size_t      removed;

    if (zbra_db_open(&db, zbra_db_default_root()) != 0)
        die("cannot open the database: %s", strerror(errno));

    if (zbra_db_get(&db, name, &e) != 1) {
        printf("%s is not installed.\n", name);
        zbra_db_close(&db);
        return;
    }

    /* Refuse while anything still depends on it. */
    if (e.n_dependents > 0) {
        printf("%s is still required by:\n", name);
        for (i = 0; i < e.n_dependents; i++)
            printf("  %s\n", e.dependents[i]);
        printf("Remove those first.\n");
        zbra_entry_free(&e);
        zbra_db_close(&db);
        exit(1);
    }

    /*
     * Only files this package owns are unlinked. The ownership check is what
     * keeps a removal from taking a config file the user has since edited --
     * or a file another package has since claimed.
     */
    removed = 0;
    for (i = 0; i < e.n_files; i++) {
        char full[4096];
        char prefix[4096];

        snprintf(prefix, sizeof(prefix), "/%s", e.files[i]);

        owner = zbra_db_owner_of(&db, prefix);
        if (owner != NULL && strcmp(owner, name) != 0) {
            printf("Keeping %s: it belongs to %s.\n", e.files[i], owner);
            free(owner);
            continue;
        }
        free(owner);

        snprintf(full, sizeof(full), "%s%s", e.install_root != NULL
                                                  ? e.install_root
                                                  : "",
                 prefix);

        if (unlink(full) != 0 && errno != ENOENT)
            fprintf(stderr, PROGNAME ": cannot remove %s: %s\n", full,
                    strerror(errno));
        else
            removed++;
    }

    zbra_db_forget(&db, name, e.kind);

    /*
     * Drop the links this package held on its dependencies.
     *
     * Without this the entry is gone but the dependency's record still claims
     * it is required, so the next `remove` of that library is refused by a
     * package that no longer exists and cannot be removed to satisfy it. The
     * database would deadlock itself one removal after the first.
     */
    for (i = 0; i < e.n_depends; i++) {
        zbra_dep_full d;

        if (e.depends[i] == NULL)
            continue;

        if (zbra_dep_parse(e.depends[i], &d) != 0)
            continue;

        if (zbra_db_unrequire(&db, d.name, name) < 0)
            fprintf(stderr, PROGNAME ": warning: cannot record that %s no "
                                    "longer needs %s: %s\n",
                    name, d.name, strerror(errno));

        zbra_dep_free(&d);
    }

    printf("Removed %s (%zu file%s).\n", name, removed, removed == 1 ? "" : "s");

    zbra_entry_free(&e);
    zbra_db_close(&db);
}

/* ---------------------------------------------------------------- verify */

static void cmd_verify(void)
{
    zbra_db    db;
    zbra_resolver res;
    char      *err = NULL;
    char     **names;
    size_t     n;
    size_t     i;
    int        problems = 0;

    if (zbra_db_open(&db, zbra_db_default_root()) != 0)
        die("cannot open the database: %s", strerror(errno));

    names = zbra_db_list(&db, ZBRA_KIND_ANY, &n);
    if (names == NULL) {
        printf("Nothing installed.\n");
        zbra_db_close(&db);
        return;
    }

    res.find_candidate    = NULL;
    res.installed_version = db_installed_version;
    res.ud                = NULL;
    res.ud_installed      = &db;

    for (i = 0; i < n && names[i] != NULL; i++) {
        zbra_entry e;

        if (zbra_db_get(&db, names[i], &e) != 1)
            continue;

        if (zbra_deps_check_installed(&res, e.name,
                                      (const char *const *)e.depends,
                                      e.n_depends, &err) != 0) {
            printf("broken: %s\n", err != NULL ? err : "unsatisfied "
                                                      "dependency");
            free(err);
            err = NULL;
            problems++;
        }

        /* A file that vanished is worth reporting before it confuses a user. */
        {
            size_t f;

            for (f = 0; f < e.n_files; f++) {
                char full[4096];

                snprintf(full, sizeof(full), "%s/%s",
                         e.install_root != NULL ? e.install_root : "", e.files[f]);
                if (access(full, F_OK) != 0) {
                    printf("missing: %s (owned by %s)\n", e.files[f], e.name);
                    problems++;
                }
            }
        }

        zbra_entry_free(&e);
    }

    if (problems == 0)
        printf("Everything checks out.\n");
    else
        printf("%d problem%s found.\n", problems, problems == 1 ? "" : "s");

    zbra_strlist_free_owned(names);
    zbra_db_close(&db);
}

/* ------------------------------------------------------------------ main */

static void usage(FILE *out)
{
    fprintf(out,
"Usage: " PROGNAME " <command> [arguments]\n"
"\n"
"TenebraOS package manager.\n"
"\n"
"Commands:\n"
"  install <pkg>          install a package and its dependencies\n"
"  remove <pkg>           remove a package and the files it owns\n"
"  search <text>          search the index (offline)\n"
"  show <pkg>             show details of an installed package\n"
"  list                   list installed packages\n"
"  update                 refresh every configured source\n"
"  verify                 check for conflicts and missing files\n"
"  sources                list configured sources\n"
"  sources add <id> <type> <url>\n"
"  sources remove <id>\n"
"  edit-sources           edit the source list in $EDITOR\n"
"\n"
"Source types: deb, rpm, tar.xz, aur, snap.\n"
"Deb, tar.xz and snap work out of the box; rpm and aur need their own\n"
"tools installed, and say so if they are missing.\n"
"\n"
"Environment:\n"
"  ZBRA_ROOT         database location (default /var/lib/zbra)\n"
"  ZBRA_CONFIG_DIR   source configuration (default /etc/zbra)\n"
"  ZBRA_CACHE_DIR    downloaded packages and the index\n"
);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "search") == 0) {
        if (argc < 3)
            die("usage: %s search <text>", PROGNAME);
        cmd_search(argv[2]);
    } else if (strcmp(argv[1], "show") == 0) {
        if (argc < 3)
            die("usage: %s show <package>", PROGNAME);
        cmd_show(argv[2]);
    } else if (strcmp(argv[1], "list") == 0) {
        cmd_list();
    } else if (strcmp(argv[1], "update") == 0) {
        cmd_update();
    } else if (strcmp(argv[1], "install") == 0) {
        if (argc < 3)
            die("usage: %s install <package>", PROGNAME);
        cmd_install(argv[2]);
    } else if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3)
            die("usage: %s remove <package>", PROGNAME);
        cmd_remove(argv[2]);
    } else if (strcmp(argv[1], "verify") == 0) {
        cmd_verify();
    } else if (strcmp(argv[1], "sources") == 0) {
        cmd_sources(argc - 2, argv + 2);
    } else if (strcmp(argv[1], "edit-sources") == 0) {
        cmd_edit_sources();
    } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("%s %s\n", PROGNAME, ZBRA_VERSION);
    } else {
        fprintf(stderr, PROGNAME ": unknown command \"%s\"\n\n", argv[1]);
        usage(stderr);
        return 1;
    }

    return 0;
}
#ifndef ZBRA_COMMIT_H
#define ZBRA_COMMIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A staged install.
 *
 * A backend unpacks a payload into zbra_commit_staging() and records where
 * each file belongs. Nothing touches the live filesystem until
 * zbra_commit_apply().
 *
 * Apply is transactional in the only sense that matters in practice: every
 * conflict is detected *before* the first move, and if a move still fails
 * part-way through, the moves already made are undone. A failed install
 * therefore leaves the system exactly as it was, rather than half-upgraded
 * with the package's files in the staging directory and its database entry
 * claiming success.
 *
 * A commit is owned by the caller and released with zbra_commit_abort(), or
 * consumed by a successful zbra_commit_apply().
 */
typedef struct zbra_commit zbra_commit;

/*
 * Create a commit whose staging directory lives under `staging_parent`
 * (normally the database root, so the eventual moves stay on one filesystem
 * and can be renames rather than copies).
 *
 * Returns 0 and stores a commit in *out, or -1 with a malloc'd reason in
 * *err (release with free).
 */
int zbra_commit_open(zbra_commit **out, const char *staging_parent,
                     char **err);

/* The directory a backend must unpack its payload into. */
const char *zbra_commit_staging(const zbra_commit *c);

/*
 * Record one file to install.
 *
 * `rel` is the payload's path relative to the staging directory, and
 * `dest_rel` where it should end up relative to the install root. Both are
 * checked for traversal and absolute paths; a backend handing over
 * "../etc/shadow" from a hostile tarball is rejected here rather than
 * trusted.
 *
 * Returns 0, or -1 with a reason in *err.
 */
int zbra_commit_add(zbra_commit *c, const char *rel, const char *dest_rel,
                    char **err);

/* How many files are staged. */
size_t zbra_commit_count(const zbra_commit *c);

/*
 * Move every staged file into `install_root`.
 *
 * On success the commit is consumed and *c is set to NULL. On failure the
 * commit is left intact so the caller can inspect or abort it, and *err
 * explains what went wrong.
 */
int zbra_commit_apply(zbra_commit **c, const char *install_root, char **err);

/* Throw the staged install away, removing the staging directory. */
void zbra_commit_abort(zbra_commit *c);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_COMMIT_H */
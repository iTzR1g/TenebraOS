#ifndef ZBRA_BACKEND_H
#define ZBRA_BACKEND_H

#include <stddef.h>

#include "commit.h"
#include "index.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A payload backend: it turns an already-downloaded package file into staged
 * files inside a zbra_commit.
 *
 * The division of labour is deliberate. Backends do not resolve dependencies,
 * do not fetch, and do not touch the database, so adding a package format means
 * writing one small file here rather than changing everything.
 *
 * Two flavours:
 *
 *   Native  zbra unpacks the payload itself. tar.xz is the format TenebraOS
 *           publishes, and unpacking it directly means installing a TenebraOS
 *           package never depends on another package manager being present.
 *           (Decompression is still tar(1) and xz(1) rather than a bundled
 *           codec: those are core tools, not package managers, and shipping a
 *           second xz implementation would be a far worse trade than calling
 *           the system's.)
 *
 *   Delegated  The format belongs to another tool with a large corpus of
 *           nasty real-world edge cases: rpm, Arch's PKGBUILD, snap. Rather
 *           than reimplementing unpacking, checksums and post-install
 *           scripting, zbra runs that tool and adopts its output. The
 *           behaviour then matches what the user would get from the native
 *           tool, which is what they expect when a package declares itself
 *           rpm or aur.
 */

/* Does this backend handle the given package? */
int zbra_backend_claims(const char *b, const char *format,
                        const char *filename);

/*
 * Stage `path` into the commit.
 *
 * `pkg` supplies the format and, for tar.xz, may carry an explicit payload
 * path when the index disagrees with the archive's own layout. Returns 0, or
 * -1 with a malloc'd reason in *err.
 */
int zbra_backend_stage(const char *b, zbra_commit *c, const char *path,
                       const zbra_package *pkg, char **err);

/* Whether the backend can run here at all (a delegated tool is present). */
int zbra_backend_available(const char *b, char **why);

/* Every backend zbra knows about, as a NULL-terminated list of names. */
const char *const *zbra_backend_names(void);

/*
 * Record everything under `prefix` in the staging directory as install
 * targets, keeping the layout. Used by the delegated backends, whose tools do
 * the unpacking themselves, and by the tar backend for its own walk.
 *
 * Returns 0, or -1 with a reason in *err. Anything that is not a regular file,
 * directory or safe symlink is refused rather than installed.
 */
int zbra_backend_adopt(zbra_commit *c, const char *prefix, char **err);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_BACKEND_H */
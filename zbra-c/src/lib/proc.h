#ifndef ZBRA_PROC_H
#define ZBRA_PROC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Running child processes.
 *
 * Every external tool zbra delegates to is invoked through here, and there is
 * deliberately no way to pass a command line as a string. Package names, URLs
 * and paths all come from index files that a third party wrote, so anything
 * that reached a shell would be a command injection; going straight to
 * execv(2) with an argument vector makes that class of bug unrepresentable
 * rather than merely avoided.
 */

/*
 * Run `argv` and wait for it.
 *
 * `out` receives the child's standard output (NUL-terminated, release with
 * free) and `status` its exit status, or NULL to skip either. Standard error
 * is inherited, so a failing tool explains itself in the user's terminal
 * instead of being swallowed.
 *
 * Returns 0 when the child ran, regardless of how it exited, or -1 with a
 * malloc'd reason in *err when it could not be started at all.
 */
int zbra_run(const char *const argv[], char **out, int *status, char **err);

/*
 * As zbra_run, but standard error is captured too and included in *err when
 * the child fails. Used where a tool's diagnostics are the only useful error
 * message.
 */
int zbra_run_capture(const char *const argv[], char **out, char **err,
                     int *status);

/*
 * Run `argv` in `dir`, optionally reading its standard input from `stdin_path`.
 *
 * Both are needed by the delegated backends: makepkg must run inside the build
 * directory it was handed, and cpio reads its archive on stdin. Either may be
 * NULL to inherit.
 */
int zbra_run_at(const char *dir, const char *stdin_path,
                const char *const argv[], char **out, char **err, int *status);

/*
 * Run `argv` with its standard output written straight to `path`.
 *
 * Needed for anything that emits binary data: a .deb's data member, for
 * instance, cannot go through a C string buffer, because the payload contains
 * NUL bytes and the length would be lost.
 */
int zbra_run_to_file(const char *const argv[], const char *path, char **err,
                     int *status);

/* Whether `name` is on PATH and runnable. */
int zbra_have_program(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_PROC_H */
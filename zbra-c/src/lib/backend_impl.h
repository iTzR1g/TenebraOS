#ifndef ZBRA_BACKEND_IMPL_H
#define ZBRA_BACKEND_IMPL_H

/*
 * The interface each backend file implements.
 *
 * Internal to the backend set: src/lib/backend.c owns the table and dispatch.
 * Keeping the shape in a header rather than in the table file lets a backend
 * be tested on its own and keeps backend.c about routing.
 */

#include <stddef.h>

#include "commit.h"
#include "index.h"

/*
 * `available` returns 1 when the backend can do anything useful on this
 * machine, or 0 with a malloc'd explanation in *why. It may be NULL for a
 * backend with no external dependency.
 */
typedef struct {
    const char *name;      /* the format string used in the database */
    const char *label;     /* human-readable, for messages */
    int  (*claims)(const char *format, const char *filename);
    int  (*stage)(zbra_commit *c, const char *path, const zbra_package *pkg,
                  char **err);
    int  (*available)(char **why);
} zbra_backend_impl;

/* ------------------------------------------------------------- tar.xz */

int zbra_backend_tar_claims(const char *format, const char *filename);
int zbra_backend_tar_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err);
int zbra_backend_tar_available(char **why);

/* ---------------------------------------------------------------- deb */

int zbra_backend_deb_claims(const char *format, const char *filename);
int zbra_backend_deb_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err);
int zbra_backend_deb_available(char **why);

/* ---------------------------------------------------------------- rpm */

int zbra_backend_rpm_claims(const char *format, const char *filename);
int zbra_backend_rpm_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err);
int zbra_backend_rpm_available(char **why);

/* ---------------------------------------------------------------- aur */

int zbra_backend_aur_claims(const char *format, const char *filename);
int zbra_backend_aur_stage(zbra_commit *c, const char *path,
                           const zbra_package *pkg, char **err);
int zbra_backend_aur_available(char **why);

/* --------------------------------------------------------------- snap */

int zbra_backend_snap_claims(const char *format, const char *filename);
int zbra_backend_snap_stage(zbra_commit *c, const char *path,
                            const zbra_package *pkg, char **err);
int zbra_backend_snap_available(char **why);

#endif /* ZBRA_BACKEND_IMPL_H */
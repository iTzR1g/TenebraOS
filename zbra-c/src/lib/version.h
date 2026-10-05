#ifndef ZBRA_VERSION_H
#define ZBRA_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The release version, supplied by the build from the VERSION file.
 *
 * ZBRA_VERSION is defined in CFLAGS. The fallback exists so that compiling a
 * single file by hand still works; a build that forgot to pass it should not
 * also fail to compile.
 */
#ifndef ZBRA_VERSION
#define ZBRA_VERSION "0.0.0-dev"
#endif

/* The version of the database format, bumped when the layout changes. */
#define ZBRA_DB_FORMAT 1

#ifdef __cplusplus
}
#endif

#endif /* ZBRA_VERSION_H */

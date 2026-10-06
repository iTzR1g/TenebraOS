/*
 * SHA-256, for verifying that a payload is the file the index promised.
 *
 * This exists rather than a call into libcrypto because a package manager
 * whose only link-time dependency is libc can be built into any image,
 * including the recovery environment where the thing being verified is
 * the one that would have to fix it.
 *
 * The implementation is FIPS 180-4. It is used here only to compare
 * digests, never to authenticate, so there is no length-extension concern:
 * a package name, not a MAC, is what a digest stands in for.
 */
#ifndef ZBRA_SHA256_H
#define ZBRA_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define ZBRA_SHA256_DIGEST_LEN 32
#define ZBRA_SHA256_HEX_LEN    64 /* not counting the terminator */

typedef struct {
    uint32_t state[8];
    uint64_t bits;
    unsigned char buf[64];
    size_t      used;
} zbra_sha256;

/* Returns 0 on success, -1 on allocation failure. */
int  zbra_sha256_init(zbra_sha256 *c);
void zbra_sha256_update(zbra_sha256 *c, const void *data, size_t len);
void zbra_sha256_final(zbra_sha256 *c, unsigned char out[ZBRA_SHA256_DIGEST_LEN]);

/* One-shot convenience wrapper. */
void zbra_sha256_buf(const void *data, size_t len,
                     unsigned char out[ZBRA_SHA256_DIGEST_LEN]);

/*
 * Hex-encodes a digest into out, which must hold ZBRA_SHA256_HEX_LEN
 * characters plus a terminator. Lower case, which is what every index
 * format in use writes.
 */
void zbra_sha256_hex(const unsigned char digest[ZBRA_SHA256_DIGEST_LEN],
                     char *out);

/*
 * Hashes the file at path. Returns 0 and writes hex on success, or -1 with
 * errno set. A directory, a missing file and a read error all fail; none of
 * them produce a digest that could be mistaken for a verified payload.
 */
int zbra_sha256_file(const char *path, char *hex);

/*
 * Compares a computed hex digest against an index's "sha256:<hex>" field.
 *
 * Returns 0 when the file matches, -1 when it does not, and -2 when the
 * recorded checksum cannot be understood -- which is deliberately not the
 * same as a mismatch. An index entry with a checksum in a format zbra
 * cannot verify has not been verified, and treating that as a pass would
 * make the field optional in practice.
 *
 * hex must be exactly ZBRA_SHA256_HEX_LEN lower-case hex characters.
 * The comparison is not constant time; the digest is public, so there is
 * nothing to learn from the time it takes.
 */
int zbra_sha256_check(const char *path, const char *checksum);

/* True if s is exactly 64 lower-case hex characters. Exposed for tests. */
int zbra_sha256_is_hex(const char *s);

#endif /* ZBRA_SHA256_H */
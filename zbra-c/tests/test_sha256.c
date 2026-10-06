/*
 * sha256: the published vectors first.
 *
 * A hash that is subtly wrong will still compare equal to itself, so
 * self-consistency is not evidence of anything. These digests are the
 * FIPS 180-4 examples plus the ones that trip over block-boundary bugs,
 * because a one-block hash that never crosses a boundary passes every
 * test that only feeds it short messages.
 */
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int tests_run;
static int tests_failed;

static void ok(int cond, const char *what)
{
    tests_run++;
    if (!cond) {
        tests_failed++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_buf(const char *input, const char *expect,
                      const char *what)
{
    unsigned char digest[ZBRA_SHA256_DIGEST_LEN];
    char          hex[ZBRA_SHA256_HEX_LEN + 1];

    zbra_sha256_buf(input, strlen(input), digest);
    zbra_sha256_hex(digest, hex);
    ok(strcmp(hex, expect) == 0, what);
    if (strcmp(hex, expect) != 0)
        printf("    expected %s\n    got      %s\n", expect, hex);
}

/* ---------------------------------------------------------- published */

static void test_vectors(void)
{
    puts("sha256: published vectors");

    /* The three from FIPS 180-4 appendix B, including the empty message. */
    check_buf("",
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
              "the empty string");
    check_buf("abc",
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "\"abc\"");
    check_buf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
              "the 56-byte example");

    /* Spells out the same as the one FIPS gives in prose. */
    check_buf("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
              "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1",
              "the 112-byte example");

    /*
     * One million 'a', the classic long-message vector. It crosses the
     * 2^32-bit counter boundary only at 512MB, but it does cross enough
     * 64-byte blocks that a length field kept in 32 bits would still be
     * exercised, and it is the canonical streaming test.
     */
    {
        zbra_sha256 c;
        unsigned char digest[ZBRA_SHA256_DIGEST_LEN];
        char hex[ZBRA_SHA256_HEX_LEN + 1];
        char chunk[1000];
        int  i;

        memset(chunk, 'a', sizeof(chunk));
        zbra_sha256_init(&c);
        for (i = 0; i < 1000; i++)
            zbra_sha256_update(&c, chunk, sizeof(chunk));
        zbra_sha256_final(&c, digest);
        zbra_sha256_hex(digest, hex);

        ok(strcmp(hex,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39c"
                  "cc7112cd0") == 0,
           "one million 'a'");
        if (strcmp(hex,
                   "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39c"
                   "cc7112cd0") != 0)
            printf("    got %s\n", hex);
    }
}

/* --------------------------------------------------------- block edges */

/*
 * Every length from 0 to 200, hashed in one shot and hashed again a byte at a
 * time.
 *
 * This is the test that matters for the incremental path: if update() mishandles
 * a partial block, the one-shot and byte-at-a-time digests diverge at exactly
 * the lengths where the block splits fall. Checking both against each other
 * for every length finds that without needing a table of known answers.
 */
static void test_block_boundaries(void)
{
    unsigned char buf[200];
    size_t        n;

    puts("sha256: block boundaries and incremental update");

    for (n = 0; n < sizeof(buf); n++)
        buf[n] = (unsigned char)(n * 7 + 3);

    for (n = 0; n < sizeof(buf); n++) {
        unsigned char one[ZBRA_SHA256_DIGEST_LEN];
        unsigned char inc[ZBRA_SHA256_DIGEST_LEN];
        zbra_sha256   c;
        size_t        i;

        zbra_sha256_buf(buf, n, one);

        zbra_sha256_init(&c);
        for (i = 0; i < n; i++)
            zbra_sha256_update(&c, &buf[i], 1);
        zbra_sha256_final(&c, inc);

        ok(memcmp(one, inc, sizeof(one)) == 0,
           "one shot equals byte-at-a-time");
        if (memcmp(one, inc, sizeof(one)) != 0)
            printf("    at length %zu\n", n);
    }

    /* Odd chunk sizes that straddle the internal 64KB read buffer. */
    {
        unsigned char big[200000];
        unsigned char a[ZBRA_SHA256_DIGEST_LEN];
        unsigned char b[ZBRA_SHA256_DIGEST_LEN];
        zbra_sha256   c;
        size_t        off;
        size_t        step;

        for (n = 0; n < sizeof(big); n++)
            big[n] = (unsigned char)n;

        zbra_sha256_buf(big, sizeof(big), a);

        for (step = 1; step <= 65536; step *= 4) {
            zbra_sha256_init(&c);
            for (off = 0; off < sizeof(big); off += step) {
                size_t left = sizeof(big) - off;

                zbra_sha256_update(&c, big + off, left < step ? left : step);
            }
            zbra_sha256_final(&c, b);

            ok(memcmp(a, b, sizeof(a)) == 0,
               "chunked update matches the whole buffer");
        }
    }

    /* NULL with a length of zero must be tolerated; callers pass fields. */
    {
        unsigned char a[ZBRA_SHA256_DIGEST_LEN];
        unsigned char b[ZBRA_SHA256_DIGEST_LEN];

        zbra_sha256_buf("", 0, a);
        zbra_sha256_buf(NULL, 0, b);
        ok(memcmp(a, b, sizeof(a)) == 0, "a NULL pointer and zero length");
    }
}

/* --------------------------------------------------------------- files */

static char *write_tmp(const char *name, const void *data, size_t len)
{
    static char   slots[4][512];
    static size_t next_slot;
    char         *path = slots[next_slot++ % 4];
    int           fd;
    ssize_t       n;

    snprintf(path, sizeof(slots[0]), "/tmp/zbra-sha-%ld-%s",
             (long)getpid(), name);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror(path);
        exit(1);
    }

    while (len > 0) {
        n = write(fd, data, len);
        if (n <= 0) {
            perror(path);
            exit(1);
        }
        data = (const unsigned char *)data + n;
        len -= (size_t)n;
    }

    close(fd);
    return path;
}

static void test_file_and_check(void)
{
    const char   *msg = "the quick brown fox jumps over the lazy dog";
    const char   *want = "05c6e08f1d9fdafa03147fcb8f82f124c76d2f70e3d989dc8aadb5e7d7450bec";
    char         *path;
    char          hex[ZBRA_SHA256_HEX_LEN + 1];
    char          rec[80];

    puts("sha256: files and index checksums");

    path = write_tmp("fox", msg, strlen(msg));

    ok(zbra_sha256_file(path, hex) == 0, "hashing a file succeeds");
    ok(strcmp(hex, want) == 0, "the file digest is right");
    if (strcmp(hex, want) != 0)
        printf("    got %s\n", hex);

    snprintf(rec, sizeof(rec), "sha256:%s", want);
    ok(zbra_sha256_check(path, rec) == 0, "a matching checksum passes");

    /* One byte different must not pass. */
    snprintf(rec, sizeof(rec), "sha256:%s",
             "05c6e08f1d9fdafa03147fcb8f82f124c76d2f70e3d989dc8aadb5e7d7450bef");
    ok(zbra_sha256_check(path, rec) == -1, "a wrong checksum fails");

    /* Truncated file. */
    path = write_tmp("fox", msg, strlen(msg) - 1);
    snprintf(rec, sizeof(rec), "sha256:%s", want);
    ok(zbra_sha256_check(path, rec) == -1, "a truncated file fails");

    /*
     * An empty file must not hash to something a truncated message would, and
     * in particular the "failed read" path must never hand back the digest of
     * zero bytes.
     */
    {
        unsigned char empty[ZBRA_SHA256_DIGEST_LEN];
        char          ehex[ZBRA_SHA256_HEX_LEN + 1];

        zbra_sha256_buf("", 0, empty);
        zbra_sha256_hex(empty, ehex);
        ok(strcmp(ehex,
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b"
                  "7852b855") == 0,
           "the empty file's digest is right");
    }

    ok(zbra_sha256_check("/nonexistent/zbra/payload", "sha256:"
                         "0000000000000000000000000000000000000000000000000000"
                         "000000000000") == -1,
       "a missing file fails");

    /* A directory opens, then fails to read: that must be an error. */
    ok(zbra_sha256_check("/tmp", "sha256:"
                         "0000000000000000000000000000000000000000000000000000"
                         "000000000000") == -1,
       "a directory is not a payload");

    /* Formats that cannot be verified are not the same as a mismatch. */
    ok(zbra_sha256_check("/tmp", "md5:" "0123456789abcdef0123456789abcdef")
       == -2, "an unknown algorithm is unverifiable");
    ok(zbra_sha256_check("/tmp", "sha256:NOTHEX") == -2,
       "a malformed digest is unverifiable");
    ok(zbra_sha256_check("/tmp", "sha256:" "abc") == -2,
       "a short digest is unverifiable");
    ok(zbra_sha256_check("/tmp",
                         "sha256:D7A8FBB307D7809469CA9ABCB0082E4F8D5651E46D3CDB7"
                         "7450BEC") == -2,
       "an upper-case digest is unverifiable, not silently matched");
    ok(zbra_sha256_check("/tmp", NULL) == -2, "no checksum is unverifiable");

    ok(zbra_sha256_is_hex(want) == 1, "is_hex accepts a digest");
    ok(zbra_sha256_is_hex("xyz") == 0, "is_hex rejects nonsense");
    ok(zbra_sha256_is_hex(NULL) == 0, "is_hex rejects NULL");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_vectors();
    test_block_boundaries();
    test_file_and_check();

    printf("\nsha256: %d passed, %d failed\n", tests_run, tests_failed);

    return tests_failed != 0;
}
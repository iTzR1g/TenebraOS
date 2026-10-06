/*
 * SHA-256 per FIPS 180-4.
 *
 * Written out rather than pulled in because a package manager that needs a
 * crypto library to check a checksum cannot bootstrap itself: the image that
 * is missing its crypto library is exactly the image that cannot repair it.
 * The whole construction is about 150 lines of table lookup and the test
 * suite checks it against the published vectors, so there is nothing here a
 * reader has to trust that a test has not already checked.
 */
#include "sha256.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_block(zbra_sha256 *c, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h;
    int      i;

    for (i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];

    for (i = 16; i < 64; i++)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];

    a = c->state[0]; b = c->state[1]; cc = c->state[2]; d = c->state[3];
    e = c->state[4]; f = c->state[5]; g  = c->state[6]; h = c->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + BSIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = BSIG0(a) + MAJ(a, b, cc);

        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g;  c->state[7] += h;
}

int zbra_sha256_init(zbra_sha256 *c)
{
    if (c == NULL)
        return -1;

    c->state[0] = 0x6a09e667u; c->state[1] = 0xbb67ae85u;
    c->state[2] = 0x3c6ef372u; c->state[3] = 0xa54ff53au;
    c->state[4] = 0x510e527fu; c->state[5] = 0x9b05688cu;
    c->state[6] = 0x1f83d9abu; c->state[7] = 0x5be0cd19u;
    c->bits  = 0;
    c->used  = 0;

    return 0;
}

void zbra_sha256_update(zbra_sha256 *c, const void *data, size_t len)
{
    const unsigned char *p = data;

    if (c == NULL || (p == NULL && len > 0))
        return;

    c->bits += (uint64_t)len * 8;

    /* Top up a partial block before switching to whole-block updates. */
    if (c->used > 0) {
        size_t want = 64 - c->used;
        size_t take = len < want ? len : want;

        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p       += take;
        len     -= take;

        if (c->used < 64)
            return;

        sha256_block(c, c->buf);
        c->used = 0;
    }

    while (len >= 64) {
        sha256_block(c, p);
        p   += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(c->buf, p, len);
        c->used = len;
    }
}

void zbra_sha256_final(zbra_sha256 *c, unsigned char out[ZBRA_SHA256_DIGEST_LEN])
{
    uint64_t bits = c->bits;
    int      i;

    /*
     * The length is appended to the same block as the padding, so it has to be
     * captured before the padding is added rather than counted from the
     * padded message.
     *
     * used is at most 63 here, since update() only ever holds a partial block,
     * so appending the 0x80 cannot run off the end of the buffer.
     */
    c->buf[c->used++] = 0x80;

    if (c->used > 56) {
        while (c->used < 64)
            c->buf[c->used++] = 0x00;
        sha256_block(c, c->buf);
        c->used = 0;
    }

    while (c->used < 56)
        c->buf[c->used++] = 0x00;

    for (i = 0; i < 8; i++)
        c->buf[56 + i] = (unsigned char)(bits >> (56 - 8 * i));

    sha256_block(c, c->buf);
    c->used = 0;

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(c->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->state[i];
    }
}

void zbra_sha256_buf(const void *data, size_t len,
                     unsigned char out[ZBRA_SHA256_DIGEST_LEN])
{
    zbra_sha256 c;

    zbra_sha256_init(&c);
    zbra_sha256_update(&c, data, len);
    zbra_sha256_final(&c, out);
}

void zbra_sha256_hex(const unsigned char digest[ZBRA_SHA256_DIGEST_LEN],
                     char *out)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < ZBRA_SHA256_DIGEST_LEN; i++) {
        out[i * 2]     = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }

    out[ZBRA_SHA256_HEX_LEN] = '\0';
}

int zbra_sha256_is_hex(const char *s)
{
    int i;

    if (s == NULL)
        return 0;

    for (i = 0; i < ZBRA_SHA256_HEX_LEN; i++) {
        char ch = s[i];

        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return 0;
    }

    return s[ZBRA_SHA256_HEX_LEN] == '\0';
}

int zbra_sha256_file(const char *path, char *hex)
{
    zbra_sha256  c;
    unsigned char digest[ZBRA_SHA256_DIGEST_LEN];
    unsigned char buf[65536];
    int          fd;
    ssize_t      n;

    if (path == NULL || hex == NULL)
        return -1;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    /*
     * A directory opens fine and then fails to read. That has to be an error
     * rather than the digest of zero bytes, or a truncated or wrong-typed
     * file could be made to match an "empty" checksum.
     */
    zbra_sha256_init(&c);

    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0)
            goto fail;
        if (n == 0)
            break;
        zbra_sha256_update(&c, buf, (size_t)n);
    }

    close(fd);

    zbra_sha256_final(&c, digest);
    zbra_sha256_hex(digest, hex);

    return 0;

fail:
    close(fd);
    return -1;
}

int zbra_sha256_check(const char *path, const char *checksum)
{
    char        hex[ZBRA_SHA256_HEX_LEN + 1];
    const char *want;

    if (path == NULL || checksum == NULL)
        return -2;

    if (strncmp(checksum, "sha256:", 7) != 0)
        return -2;

    want = checksum + 7;

    if (!zbra_sha256_is_hex(want))
        return -2;

    if (zbra_sha256_file(path, hex) != 0)
        return -1;

    return strcmp(hex, want) == 0 ? 0 : -1;
}
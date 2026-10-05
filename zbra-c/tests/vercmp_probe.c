/*
 * vercmp_probe.c -- differential fuzz harness for the Debian comparator.
 *
 * Emits pseudo-random *dpkg-valid* version strings, one pair per line, in
 * the form "<left>\t<right>". The caller (fuzz_deb.sh) asks dpkg
 * --compare-versions what it thinks and compares that against
 * zbra_version_cmp. A fixed seed makes failures reproducible.
 *
 * Generating only valid versions matters: dpkg rejects anything with an
 * empty epoch, an empty post-colon component, or an upstream that does not
 * begin with a digit, exiting with status 2 rather than an answer. Feeding
 * those to the comparison would compare an error against a real verdict.
 * The generator below therefore respects dpkg's grammar, while still
 * covering every branch of its ordering (tilde, digits, upper/lower case
 * letters, punctuation that outranks letters, epoch, revision).
 */

#include "vercmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic PRNG so a failing seed can be replayed exactly. */
static unsigned long rng_state = 123456789UL;

static unsigned long rng_next(void)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (rng_state >> 16) & 0x7fff;
}

static const char body_alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz"
                                    "ABCDEFGHIJKLMNOPQRSTUVWXYZ.+~";

/*
 * Build one dpkg-valid version:
 *
 *   [digits ':'] digit+ ( alnum | '.' | '+' | '~' )* [ '-' body ]
 *
 * The upstream part always starts with a digit (dpkg requires it) and an
 * epoch, when present, is a non-empty run of digits.
 */
static void gen(char *buf, size_t cap)
{
    size_t pos = 0;
    size_t i, n;

#define APPEND(c) do { if (pos + 1 < cap) buf[pos++] = (char)(c); } while (0)

    /* Optional epoch. */
    if ((rng_next() % 4) == 0) {
        n = 1 + (rng_next() % 2);
        for (i = 0; i < n; i++)
            APPEND('0' + (rng_next() % 10));
        APPEND(':');
    }

    /* Upstream: leading digit, then a few arbitrary body characters. */
    APPEND('0' + (rng_next() % 10));
    n = rng_next() % 8;
    for (i = 0; i < n; i++)
        APPEND(body_alphabet[rng_next() % (sizeof(body_alphabet) - 1)]);

    /* Optional Debian revision. */
    if ((rng_next() % 3) == 0) {
        APPEND('-');
        n = 1 + (rng_next() % 3);
        for (i = 0; i < n; i++)
            APPEND(body_alphabet[rng_next() % (sizeof(body_alphabet) - 1)]);
    }

    buf[pos] = '\0';
#undef APPEND
}

int main(int argc, char **argv)
{
    unsigned long n = (argc > 1) ? strtoul(argv[1], NULL, 10) : 2000;
    unsigned long i;
    char a[32], b[32];

    /* Optional second argument selects the PRNG seed, so a fuzz failure can
     * be replayed exactly with the same sequence of version strings. */
    if (argc > 2)
        rng_state = strtoul(argv[2], NULL, 10);

    for (i = 0; i < n; i++) {
        gen(a, sizeof(a));
        gen(b, sizeof(b));
        printf("%s\t%s\n", a, b);
    }

    return 0;
}

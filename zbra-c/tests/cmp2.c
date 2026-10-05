/*
 * cmp2.c -- minimal CLI wrapper around zbra_version_cmp.
 *
 * The differential fuzz script needs a verdict per version pair that dpkg
 * itself cannot supply, so it shells out to this rather than linking
 * against the library. Build-only: not part of the installed package.
 */
#include "vercmp.h"

#include <stdio.h>

int main(int argc, char **argv)
{
	int g;

	if (argc < 3) {
		fprintf(stderr, "usage: %s VERSION VERSION\n", argv[0]);
		return 2;
	}

	g = zbra_version_cmp(argv[1], argv[2], ZBRA_VER_DEB);

	/* Normalise to -1/0/1 so the script can compare strings directly. */
	printf("%d\n", (g > 0) - (g < 0));

	return 0;
}
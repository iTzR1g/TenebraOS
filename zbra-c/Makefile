# zbra-c -- universal package manager for TenebraOS
#
# Design notes relevant to building:
#
#   * Everything under src/lib is pure C with no external dependencies, so it
#     builds anywhere a C11 compiler exists. That covers version comparison,
#     index parsing, the package database, dependency resolution and source
#     management.
#
#   * Payload handling deliberately shells out to the tools that already do
#     those jobs correctly -- dpkg-deb and ar for .deb, rpm2cpio and dnf for
#     RPM, xz and tar for tarballs, makepkg for the AUR, snapd for snaps.
#     Reimplementing RPM header parsing, the libsolv SAT solver, or squashfs
#     mounting in C would be strictly worse than binding the originals.
#
#   * Consequently the only link-time requirement is libc. libcurl is used
#     when present and there is a plain-curl fallback; see fetch.c.

CC       ?= cc
CSTD     ?= -std=c11
OPT      ?= -O2 -g
WARN     := -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
            -Wmissing-prototypes -Wno-unused-parameter
CPPFLAGS += -D_GNU_SOURCE -Isrc -Isrc/lib
CFLAGS   ?= $(OPT) $(CSTD) $(WARN)
LDFLAGS  ?=
LDLIBS   ?=

LIB_SRC  := $(sort $(wildcard src/lib/*.c))
LIB_OBJ  := $(LIB_SRC:.c=.o)
CLI_SRC  := $(sort $(wildcard src/*.c))
CLI_OBJ  := $(CLI_SRC:.c=.o)

# tests/test_vercmp.c -> module name "vercmp" -> tests/bin-vercmp
TEST_SRC   := $(sort $(wildcard tests/test_*.c))
TEST_NAMES := $(patsubst tests/test_%.c,%,$(TEST_SRC))
TEST_BINS  := $(addprefix tests/bin-,$(TEST_NAMES))

.PHONY: all clean test check install uninstall fuzz

all: zbra

zbra: $(LIB_OBJ) $(CLI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Each test file has its own main(), so each becomes a separate binary that
# links the library objects. % matches the bare module name.
tests/bin-%: tests/test_%.c $(LIB_OBJ)
	@mkdir -p tests
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB_OBJ) $(LDFLAGS) $(LDLIBS)

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
		printf '%-24s ' "$$(basename $$t)"; \
		if ./$$t > /tmp/zbra-test.$$$$.log 2>&1; then \
			tail -1 /tmp/zbra-test.$$$$.log; \
		else \
			echo "FAILED"; cat /tmp/zbra-test.$$$$.log; fail=1; \
		fi; \
		rm -f /tmp/zbra-test.$$$$.log; \
	done; \
	exit $$fail

# Same tests under AddressSanitizer, UndefinedBehaviorSanitizer and
# LeakSanitizer. A package manager that corrupts memory while parsing an index
# is a remote-code-execution bug, so this is part of `make check`, not an
# optional extra.
check: $(TEST_BINS)
	@mkdir -p tests
	@fail=0; \
	for n in $(TEST_NAMES); do \
		printf 'sanitize %-20s ' "$$n"; \
		if $(CC) -O1 -g $(CSTD) $(WARN) $(CPPFLAGS) \
			-fsanitize=address,undefined -fno-omit-frame-pointer \
			-o tests/san-$$n tests/test_$$n.c $(LIB_SRC) \
			> /dev/null 2>&1 && \
		   ASAN_OPTIONS=detect_leaks=1 ./tests/san-$$n \
			> /tmp/zbra-san.$$$$.log 2>&1; then \
			tail -1 /tmp/zbra-san.$$$$.log; \
		else \
			echo "FAILED"; cat /tmp/zbra-san.$$$$.log; fail=1; \
		fi; \
		rm -f /tmp/zbra-san.$$$$.log; \
	done; \
	exit $$fail

# Differential fuzz of the Debian comparator against dpkg itself.
fuzz: tests/bin-vercmp
	@$(CC) $(CFLAGS) $(CPPFLAGS) -o tests/probe tests/vercmp_probe.c \
		$(LIB_OBJ)
	@sh tests/fuzz_deb.sh

install: zbra
	install -D -m 0755 zbra $(DESTDIR)/usr/local/bin/zbra

uninstall:
	rm -f $(DESTDIR)/usr/local/bin/zbra

clean:
	rm -f $(LIB_OBJ) $(CLI_OBJ) zbra
	rm -rf tests/bin-* tests/san-*
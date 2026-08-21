# librei — the canonical (and only) build: static and shared libraries,
# the test tiers, bench, and install. Consumers integrate the vendored
# sources or the amalgamation (tools/amalgamate.sh) with their own build
# system. Windows builds via tools/build-win.bat (clang-cl).

CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local
DESTDIR ?=

VERSION_MAJOR := 0
VERSION_MINOR := 1
VERSION_PATCH := 0

CPPFLAGS += -Iinclude -Isrc
CFLAGS   ?= -O2
CFLAGS   += -std=c11 -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden
LDFLAGS  ?=

UNAME := $(shell uname -s)

# Every TU is platform-guarded internally, so all sources compile
# everywhere (a foreign platform's file is empty).
SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

STATIC := librei.a

# REI_API defaults to empty (static linkage — vendored, amalgamated,
# MinGW — needs no macro); the object sets feeding the shared library
# define REI_SHARED for the visibility attribute.
ifeq ($(UNAME),Darwin)
  CFLAGS   += -fPIC
  CPPFLAGS += -DREI_SHARED
  SHARED  := librei.$(VERSION_MAJOR).dylib
  SONAME_FLAG := -Wl,-install_name,$(PREFIX)/lib/librei.$(VERSION_MAJOR).dylib
  SHARED_LD := -dynamiclib $(SONAME_FLAG)
  SYMLINKS := librei.dylib
else ifneq (,$(filter MINGW% UCRT% CLANG%,$(UNAME)))
  # MSYS2/MinGW (the R package's Rtools toolchain): static lib + test
  # tiers only — the Windows DLL is tools/build-win.bat's job (clang-cl).
  SHARED  :=
  SYMLINKS :=
else
  # -fPIC: the one object set serves both libs; x86_64 ld refuses
  # non-PIC (TLS slot in err.c) in the shared link. Not MinGW: PE is
  # always PIC and gcc warns on the flag.
  CFLAGS   += -fPIC
  CPPFLAGS += -DREI_SHARED
  SHARED  := librei.so.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
  SONAME_FLAG := -Wl,-soname,librei.so.$(VERSION_MAJOR)
  SHARED_LD := -shared $(SONAME_FLAG)
  SYMLINKS := librei.so.$(VERSION_MAJOR) librei.so
  LDLIBS += -pthread
endif

TEST_UNIT_SRC := $(wildcard tests/unit/*.c)
TEST_UNIT_BIN := $(TEST_UNIT_SRC:.c=)
TEST_INT_SRC  := $(wildcard tests/integration/*.c)
TEST_INT_BIN  := $(TEST_INT_SRC:.c=)
BENCH_SRC     := $(wildcard bench/*.c)
BENCH_BIN     := $(BENCH_SRC:.c=)

.PHONY: all static shared test test-unit test-integration test-soak bench \
        install uninstall clean amalgamation

all: static shared

static: $(STATIC)

shared: $(SHARED)

$(STATIC): $(OBJ)
	$(AR) rcs $@ $(OBJ)

ifneq ($(SHARED),)
$(SHARED): $(OBJ)
	$(CC) $(SHARED_LD) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)
endif

src/%.o: src/%.c include/rei.h src/internal.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Test tiers: unit (in-process, per-PR), integration (real fork/spawn
# children), soak (minutes-long contention runs; nightly). Integration
# tests compile against rei.h only — the API's compile-time contract
# check; unit tests may include internal.h.
tests/unit/%: tests/unit/%.c $(STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(STATIC) $(LDLIBS)

tests/integration/%: tests/integration/%.c $(STATIC)
	$(CC) $(CPPFLAGS) -Iinclude -std=c11 -Wall -Wextra -Wpedantic -Werror \
	  -o $@ $< $(STATIC) $(LDLIBS)

test: test-unit

test-unit: $(TEST_UNIT_BIN)
	@for t in $(TEST_UNIT_BIN); do echo "== $$t"; ./$$t || exit 1; done

test-integration: $(TEST_INT_BIN)
	@for t in $(TEST_INT_BIN); do echo "== $$t"; ./$$t || exit 1; done

test-soak: $(TEST_INT_BIN)
	@echo "no soak tier yet"

bench: $(STATIC)
	@echo "no bench suite yet"

# The amalgamation smoke test: generate rei.c + rei.h. CI compiles the
# unit tests against rei.c, then links and runs a public-only program
# against the pair with no defines — the consumer contract, and the
# proof that rei.h is self-contained (the unit test includes
# internal.h, so it cannot serve as that proof).
amalgamation:
	tools/amalgamate.sh

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/lib
	cp include/rei.h $(DESTDIR)$(PREFIX)/include/rei.h
	cp $(STATIC) $(DESTDIR)$(PREFIX)/lib/$(STATIC)
ifneq ($(SHARED),)
	cp $(SHARED) $(DESTDIR)$(PREFIX)/lib/$(SHARED)
	@for l in $(SYMLINKS); do \
	  ln -sf $(SHARED) $(DESTDIR)$(PREFIX)/lib/$$l; \
	done
endif

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/include/rei.h
	rm -f $(DESTDIR)$(PREFIX)/lib/$(STATIC) \
	      $(DESTDIR)$(PREFIX)/lib/$(SHARED) $(SYMLINKS:%=$(DESTDIR)$(PREFIX)/lib/%)

clean:
	rm -f $(OBJ) $(STATIC) $(SHARED) $(TEST_UNIT_BIN) $(TEST_INT_BIN) \
	      $(BENCH_BIN)
	rm -rf rei.c rei.h

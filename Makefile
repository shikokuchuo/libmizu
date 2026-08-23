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
  # non-PIC (TLS slot in err_tls.c) in the shared link. Not MinGW: PE is
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
TEST_SOAK_SRC := $(wildcard tests/soak/*.c)
TEST_SOAK_BIN := $(TEST_SOAK_SRC:.c=)
BENCH_SRC     := $(wildcard bench/*.c)
BENCH_BIN     := $(BENCH_SRC:.c=)

# The fuzz tier builds with clang's libFuzzer + ASan + UBSan and runs as
# short fixed-seed bursts (deterministic given seed and run count).
FUZZ_SRC  := $(wildcard tests/fuzz/*.c)
FUZZ_BIN  := $(FUZZ_SRC:.c=)
FUZZ_CC   ?= clang
FUZZ_SAN  ?= -fsanitize=fuzzer,address,undefined
FUZZ_RUNS ?= 100000
FUZZ_SEED ?= 1

.PHONY: all static shared test test-unit test-integration test-soak \
        test-fuzz bench coverage install uninstall clean amalgamation

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

# Soak: minutes-long contention runs (nightly, not per-PR). Duration in
# seconds per binary; REI_SOAK_SECONDS overrides.
SOAK_SECONDS ?= 120

tests/soak/%: tests/soak/%.c $(STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(STATIC) $(LDLIBS)

test-soak: $(TEST_SOAK_BIN)
	@for t in $(TEST_SOAK_BIN); do \
	  echo "== $$t"; \
	  REI_SOAK_SECONDS=$${REI_SOAK_SECONDS:-$(SOAK_SECONDS)} ./$$t || exit 1; \
	done

# Fuzz: libFuzzer harnesses for the parsers a crashed peer can leave
# torn (preamble validate, pool header, REF/SHM_RAW identifier). The
# library sources compile into each binary — the static lib's objects
# carry no sanitizer instrumentation.
tests/fuzz/%: tests/fuzz/%.c $(SRC)
	$(FUZZ_CC) -Iinclude -Isrc -std=c11 -Wall -Wextra -Wpedantic -Werror \
	  -g -O1 $(FUZZ_SAN) -fno-omit-frame-pointer \
	  -o $@ $< $(SRC) $(LDLIBS)

test-fuzz: $(FUZZ_BIN)
	@for f in $(FUZZ_BIN); do \
	  echo "== $$f"; \
	  ./$$f -runs=$(FUZZ_RUNS) -seed=$(FUZZ_SEED) -max_len=4096 || exit 1; \
	done

# Bench: report-only microbenchmarks (no timing asserts — runner
# variance). Records are appended to bench/notes.md with the commit SHA.
bench/%: bench/%.c $(STATIC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(STATIC) $(LDLIBS)

bench: $(BENCH_BIN)
	@for b in $(BENCH_BIN); do echo "== $$b"; ./$$b || exit 1; done

# Coverage: llvm-cov over the unit tier, report-only (line-coverage
# targets mislead on lock-free paths). Needs clang + the LLVM tools
# (via xcrun on macOS).
ifeq ($(UNAME),Darwin)
  LLVM_PROFDATA ?= xcrun llvm-profdata
  LLVM_COV      ?= xcrun llvm-cov
else
  LLVM_PROFDATA ?= llvm-profdata
  LLVM_COV      ?= llvm-cov
endif

coverage:
	$(MAKE) clean
	LLVM_PROFILE_FILE="rei-%p.profraw" $(MAKE) CC=clang \
	  CFLAGS="-O1 -g -fprofile-instr-generate -fcoverage-mapping" \
	  LDFLAGS="-fprofile-instr-generate" test-unit
	$(LLVM_PROFDATA) merge -sparse rei-*.profraw -o rei.profdata
	$(LLVM_COV) report $(firstword $(TEST_UNIT_BIN)) \
	  $(addprefix -object ,$(filter-out $(firstword $(TEST_UNIT_BIN)),$(TEST_UNIT_BIN))) \
	  -instr-profile=rei.profdata
	rm -f rei-*.profraw rei.profdata
	$(MAKE) clean   # leave no instrumented objects behind

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
	      $(TEST_SOAK_BIN) $(FUZZ_BIN) $(BENCH_BIN)
	rm -rf rei.c rei.h rei-*.profraw rei.profdata

# vek - Build System
# Requires: clang (C11), Linux x86_64

CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic
RELEASE := -O2
DEBUG   := -g -O0 -DVEK_DEBUG

SRCDIR  := src
BUILDDIR:= build
TESTDIR := tests

SRCS    := $(wildcard $(SRCDIR)/*.c)
# sqlite3.c needs special flags, so exclude it from the normal OBJS
SRCS_NO_SQLITE := $(filter-out $(SRCDIR)/sqlite3.c,$(SRCS))
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS_NO_SQLITE))
OBJS    += $(BUILDDIR)/sqlite3.o
TARGET  := $(BUILDDIR)/vek

# Test sources (each test_*.c is a standalone binary)
TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Library objects (everything except main.o, for linking with tests)
LIB_OBJS := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

.PHONY: all clean debug test test_http test_web_app asan tsan ubsan fuzz_lexer fuzz_compiler bench test_full

all: CFLAGS += $(RELEASE)
all: $(TARGET)

debug: CFLAGS += $(DEBUG)
debug: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lm -lpthread -ldl

# Special rule for sqlite3.c (suppress warnings, add defines)
# SANITIZE_FLAGS is passed in by sanitizer targets so sqlite3 gets instrumented too
$(BUILDDIR)/sqlite3.o: $(SRCDIR)/sqlite3.c | $(BUILDDIR)
	$(CC) -std=c11 -w -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -O2 $(SANITIZE_FLAGS) -c -o $@ $<

# Special rule for vm.c (suppress computed GOTO warning - intentional GNU extension for performance)
$(BUILDDIR)/vm.o: $(SRCDIR)/vm.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -Wno-gnu-label-as-value -c -o $@ $<

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Test targets
test: CFLAGS += $(DEBUG)
test: $(TARGET) $(TEST_BINS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
		else \
			echo "  FAIL $$(basename $$t)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -ne 0 ]; then \
		echo "=== $$failed unit test(s) FAILED ==="; \
		exit 1; \
	fi; \
	echo "=== All unit tests passed ==="
	@echo ""
	@echo "=== Running integration tests ==="
	@$(TESTDIR)/run_integration.sh
	@echo "=== All integration tests passed ==="

$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -I$(SRCDIR) -o $@ $< $(LIB_OBJS) -lm -lpthread -ldl

clean:
	rm -rf $(BUILDDIR)

# HTTP server integration test
test_http: $(TARGET)
	@echo "=== Running HTTP integration test ==="
	@$(TESTDIR)/test_http.sh

# Web app integration test (comprehensive end-to-end)
test_web_app: $(TARGET)
	@echo "=== Running Web App integration test ==="
	@$(TESTDIR)/test_web_app.sh

# ---- Sanitizer targets ----

# Clang resource dir for sanitizer runtime libraries.
# The system clang may not ship sanitizer runtimes; override if needed:
#   make asan CLANG_RT_DIR=/path/to/lib/clang/VERSION
CLANG_RT_DIR ?= /usr/libexec/swift/6.3/lib/clang/21

# AddressSanitizer: catches heap buffer overflows, use-after-free, double-free, leaks
asan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=address -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -resource-dir $(CLANG_RT_DIR)" _asan_build
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=address -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -resource-dir $(CLANG_RT_DIR)" _asan_test

_asan_build: $(TARGET)

_asan_test: $(TEST_BINS)
	@echo "=== Running unit tests with ASAN ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
		else \
			echo "  FAIL $$(basename $$t)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -ne 0 ]; then \
		echo "=== $$failed unit test(s) FAILED under ASAN ==="; \
		exit 1; \
	fi; \
	echo "=== All unit tests passed under ASAN ==="
	@echo ""
	@echo "=== Running integration tests with ASAN ==="
	@$(TESTDIR)/run_integration.sh

# ThreadSanitizer: catches data races
tsan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=thread -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=thread -g -resource-dir $(CLANG_RT_DIR)" _tsan_build
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=thread -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=thread -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=thread -g -resource-dir $(CLANG_RT_DIR)" _tsan_test

_tsan_build: $(TARGET)

_tsan_test: $(TEST_BINS)
	@echo "=== Running unit tests with TSAN ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
		else \
			echo "  FAIL $$(basename $$t)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -ne 0 ]; then \
		echo "=== $$failed unit test(s) FAILED under TSAN ==="; \
		exit 1; \
	fi; \
	echo "=== All unit tests passed under TSAN ==="
	@echo ""
	@echo "=== Running integration tests with TSAN ==="
	@$(TESTDIR)/run_integration.sh

# UndefinedBehaviorSanitizer: catches undefined behavior
ubsan: clean
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=undefined -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=undefined -g -resource-dir $(CLANG_RT_DIR)" _ubsan_build
	$(MAKE) CFLAGS="$(CFLAGS) -fsanitize=undefined -g -resource-dir $(CLANG_RT_DIR)" \
		LDFLAGS="-fsanitize=undefined -resource-dir $(CLANG_RT_DIR)" \
		SANITIZE_FLAGS="-fsanitize=undefined -g -resource-dir $(CLANG_RT_DIR)" _ubsan_test

_ubsan_build: $(TARGET)

_ubsan_test: $(TEST_BINS)
	@echo "=== Running unit tests with UBSAN ==="
	@failed=0; \
	for t in $(TEST_BINS); do \
		echo "  RUN  $$(basename $$t)"; \
		if $$t; then \
			echo "  PASS $$(basename $$t)"; \
		else \
			echo "  FAIL $$(basename $$t)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	if [ $$failed -ne 0 ]; then \
		echo "=== $$failed unit test(s) FAILED under UBSAN ==="; \
		exit 1; \
	fi; \
	echo "=== All unit tests passed under UBSAN ==="
	@echo ""
	@echo "=== Running integration tests with UBSAN ==="
	@$(TESTDIR)/run_integration.sh

# ---- Fuzzing targets ----

# Fuzz the lexer
fuzz_lexer: $(LIB_OBJS) | $(BUILDDIR)
	$(CC) -fsanitize=fuzzer,address -fno-omit-frame-pointer -g \
		-resource-dir $(CLANG_RT_DIR) -I$(SRCDIR) \
		-o $(BUILDDIR)/fuzz_lexer $(TESTDIR)/fuzz_lexer.c $(LIB_OBJS) -lm -lpthread -ldl -lstdc++

# Fuzz the compiler
fuzz_compiler: $(LIB_OBJS) | $(BUILDDIR)
	$(CC) -fsanitize=fuzzer,address -fno-omit-frame-pointer -g \
		-resource-dir $(CLANG_RT_DIR) -I$(SRCDIR) \
		-o $(BUILDDIR)/fuzz_compiler $(TESTDIR)/fuzz_compiler.c $(LIB_OBJS) -lm -lpthread -ldl -lstdc++

# ---- Benchmark target ----

bench: CFLAGS += $(RELEASE)
bench: $(BUILDDIR)/bench_vm
	@echo "=== Running benchmarks ==="
	@$(BUILDDIR)/bench_vm

$(BUILDDIR)/bench_vm: $(TESTDIR)/bench_vm.c $(LIB_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $< $(LIB_OBJS) -lm -lpthread -ldl

# ---- Comprehensive test target ----

test_full:
	@echo "=== Phase 1: Clean debug build + tests ==="
	$(MAKE) clean
	$(MAKE) test
	@echo ""
	@echo "=== Phase 2: ASAN build + tests ==="
	$(MAKE) asan
	@echo ""
	@echo "=== All test phases passed ==="


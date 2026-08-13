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
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
TARGET  := $(BUILDDIR)/vek

# Test sources (each test_*.c is a standalone binary)
TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# Library objects (everything except main.o, for linking with tests)
LIB_OBJS := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

.PHONY: all clean debug test test_http test_web_app

all: CFLAGS += $(RELEASE)
all: $(TARGET)

debug: CFLAGS += $(DEBUG)
debug: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lpthread -ldl

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
	$(CC) $(CFLAGS) -I$(SRCDIR) -o $@ $< $(LIB_OBJS) -lm -lpthread -ldl

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

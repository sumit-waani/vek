# vek - Build System
# Requires: clang (C11), Linux x86_64

CC      := clang
CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic
RELEASE := -O2
DEBUG   := -g -O0 -DVEK_DEBUG

SRCDIR  := src
BUILDDIR:= build
TESTDIR := tests

SRCS    := $(filter-out $(SRCDIR)/sqlite3.c,$(wildcard $(SRCDIR)/*.c))
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
OBJS    += $(BUILDDIR)/sqlite3.o
TARGET  := $(BUILDDIR)/vek

# Test sources (each test_*.c is a standalone binary)
TEST_SRCS := $(wildcard $(TESTDIR)/test_*.c)
TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(TEST_SRCS))

# vekd test sources need different linking
VEKD_TEST_SRCS := $(wildcard $(TESTDIR)/test_vekd_*.c)
VEKD_TEST_BINS := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(VEKD_TEST_SRCS))

# Non-vekd test sources link against the vek library objects
VEK_TEST_SRCS  := $(filter-out $(VEKD_TEST_SRCS),$(TEST_SRCS))
VEK_TEST_BINS  := $(patsubst $(TESTDIR)/%.c,$(BUILDDIR)/%,$(VEK_TEST_SRCS))

# Library objects (everything except main.o, for linking with tests)
LIB_OBJS := $(filter-out $(BUILDDIR)/main.o,$(OBJS))

# vekd sources (separate binary)
VEKD_SRCDIR := src/vekd
VEKD_SRCS   := $(wildcard $(VEKD_SRCDIR)/*.c)
VEKD_OBJS   := $(patsubst $(VEKD_SRCDIR)/%.c,$(BUILDDIR)/vekd_%.o,$(VEKD_SRCS))
VEKD_TARGET := $(BUILDDIR)/vekd

# Shared objects used by both vek and vekd
SHARED_OBJS := $(BUILDDIR)/sqlite3.o $(BUILDDIR)/sha256.o \
               $(BUILDDIR)/http_server.o $(BUILDDIR)/event_loop.o \
               $(BUILDDIR)/router.o $(BUILDDIR)/http_parser.o

.PHONY: all clean debug test test_http test_web_app vekd

all: CFLAGS += $(RELEASE)
all: $(TARGET)

debug: CFLAGS += $(DEBUG)
debug: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lpthread -ldl

$(BUILDDIR)/sqlite3.o: $(SRCDIR)/sqlite3.c | $(BUILDDIR)
	$(CC) -std=c11 -O2 -DSQLITE_THREADSAFE=1 -c -o $@ $<

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# vekd binary
vekd: CFLAGS += $(RELEASE)
vekd: $(VEKD_TARGET)

$(VEKD_TARGET): $(VEKD_OBJS) $(SHARED_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^ -lm -lpthread -ldl

$(BUILDDIR)/vekd_%.o: $(VEKD_SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Test targets
test: CFLAGS += $(DEBUG)
test: $(TARGET) $(VEK_TEST_BINS) $(VEKD_TEST_BINS)
	@echo "=== Running unit tests ==="
	@failed=0; \
	for t in $(VEK_TEST_BINS) $(VEKD_TEST_BINS); do \
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

$(BUILDDIR)/test_vekd_%: $(TESTDIR)/test_vekd_%.c $(VEKD_OBJS) $(SHARED_OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -I$(VEKD_SRCDIR) -o $@ $< $(filter-out $(BUILDDIR)/vekd_vekd_main.o,$(VEKD_OBJS)) $(SHARED_OBJS) -lm -lpthread -ldl

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

# ViewFS prototype build
#
# Targets:
#   all       (default) build both binaries
#   clean     remove build artifacts
#   test      run the test suite (placeholder until Phase 8)
#   help      list targets

CC       ?= gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
OPT      := -O2 -g
CPPFLAGS := -Iinclude -D_GNU_SOURCE -DFUSE_USE_VERSION=31
CFLAGS   ?= $(CSTD) $(WARN) $(OPT)
LDFLAGS  ?=

PKG_FUSE3_CFLAGS := $(shell pkg-config --cflags fuse3)
PKG_FUSE3_LIBS   := $(shell pkg-config --libs   fuse3)
PKG_LIBPQ_CFLAGS := $(shell pkg-config --cflags libpq)
PKG_LIBPQ_LIBS   := $(shell pkg-config --libs   libpq)

BUILD_DIR := build
LIB_DIR   := $(BUILD_DIR)/libviewfs
CLI_DIR   := $(BUILD_DIR)/cli
FUSE_DIR  := $(BUILD_DIR)/fuse

LIB_SRC := $(wildcard src/libviewfs/*.c)
CLI_SRC := $(wildcard src/cli/*.c)
FUSE_SRC := $(wildcard src/fuse/*.c)

LIB_OBJ  := $(patsubst src/libviewfs/%.c,$(LIB_DIR)/%.o,$(LIB_SRC))
CLI_OBJ  := $(patsubst src/cli/%.c,$(CLI_DIR)/%.o,$(CLI_SRC))
FUSE_OBJ := $(patsubst src/fuse/%.c,$(FUSE_DIR)/%.o,$(FUSE_SRC))

LIB_AR   := $(BUILD_DIR)/libviewfs.a
CLI_BIN  := viewfs
FUSE_BIN := viewfs-fuse

.PHONY: all clean test unit-test int-test install uninstall help
.DEFAULT_GOAL := all

PREFIX  ?= /usr/local
DESTDIR ?=
BINDIR  := $(DESTDIR)$(PREFIX)/bin

all: $(CLI_BIN) $(FUSE_BIN)

# libviewfs static library ----------------------------------------------------
$(LIB_DIR)/%.o: src/libviewfs/%.c | $(LIB_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_LIBPQ_CFLAGS) -c $< -o $@

$(LIB_AR): $(LIB_OBJ)
	ar rcs $@ $^

# viewfs CLI ------------------------------------------------------------------
$(CLI_DIR)/%.o: src/cli/%.c | $(CLI_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_LIBPQ_CFLAGS) -c $< -o $@

$(CLI_BIN): $(CLI_OBJ) $(LIB_AR)
	$(CC) $(CFLAGS) $(CLI_OBJ) $(LIB_AR) $(PKG_LIBPQ_LIBS) -o $@ $(LDFLAGS)

# viewfs-fuse daemon ----------------------------------------------------------
$(FUSE_DIR)/%.o: src/fuse/%.c | $(FUSE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_FUSE3_CFLAGS) $(PKG_LIBPQ_CFLAGS) -c $< -o $@

$(FUSE_BIN): $(FUSE_OBJ) $(LIB_AR)
	$(CC) $(CFLAGS) $(FUSE_OBJ) $(LIB_AR) $(PKG_FUSE3_LIBS) $(PKG_LIBPQ_LIBS) -o $@ $(LDFLAGS)

# Build directories -----------------------------------------------------------
$(LIB_DIR) $(CLI_DIR) $(FUSE_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(CLI_BIN) $(FUSE_BIN)

# Unit tests --------------------------------------------------------------
TEST_DIR  := $(BUILD_DIR)/tests
UNIT_SRC  := $(wildcard tests/unit/test_*.c)
UNIT_BIN  := $(patsubst tests/unit/%.c,$(TEST_DIR)/%,$(UNIT_SRC))

$(TEST_DIR)/%: tests/unit/%.c $(LIB_AR) | $(TEST_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_LIBPQ_CFLAGS) $< $(LIB_AR) \
	    $(PKG_LIBPQ_LIBS) -o $@ $(LDFLAGS)

$(TEST_DIR):
	mkdir -p $@

test: unit-test int-test

unit-test: $(UNIT_BIN)
	@status=0; \
	for t in $(UNIT_BIN); do \
	    echo "==> $$t"; \
	    "$$t" || status=1; \
	done; \
	exit $$status

# Integration tests need the built binaries to be in place.
INT_RUNNER := tests/integration/run.sh
int-test: $(CLI_BIN) $(FUSE_BIN)
	@if [ -x $(INT_RUNNER) ]; then \
	    echo "==> Integration suite"; \
	    $(INT_RUNNER); \
	else \
	    echo "no integration suite"; \
	fi

install: $(CLI_BIN) $(FUSE_BIN)
	install -d $(BINDIR)
	install -m 0755 $(CLI_BIN)  $(BINDIR)/$(CLI_BIN)
	install -m 0755 $(FUSE_BIN) $(BINDIR)/$(FUSE_BIN)
	@echo "Installed to $(BINDIR)"

uninstall:
	rm -f $(BINDIR)/$(CLI_BIN) $(BINDIR)/$(FUSE_BIN)
	@echo "Removed from $(BINDIR)"

help:
	@echo "Targets:"
	@echo "  all          (default) build viewfs and viewfs-fuse"
	@echo "  test         run unit + integration suites"
	@echo "  unit-test    run only the C unit tests"
	@echo "  int-test     run only the integration suite"
	@echo "  install      install both binaries to \$$PREFIX/bin (default /usr/local/bin)"
	@echo "  uninstall    remove the installed binaries"
	@echo "  clean        remove build artifacts"
	@echo
	@echo "Variables:"
	@echo "  PREFIX       install prefix (default /usr/local)"
	@echo "  DESTDIR      staging dir for packagers"

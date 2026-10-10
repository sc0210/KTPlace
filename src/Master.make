# KTPlace Master.make - Hierarchical build system for C++23
# Root makefile that coordinates all subdirectories (named Master.make for
# consistency with the per-component Master.make files)

# ============================================================================
# Configuration
# ============================================================================

# Compiler settings
CXX := g++

# `make coverage COVERAGE=1` swaps this object tree for an instrumented one, so a
# coverage run cannot leave .gcno/.gcda files next to the normal build's objects
# and then report the ordinary binary as instrumented. -O0 because gcov maps
# lines back through the optimizer at higher levels, which makes the uncovered
# set harder to read and the numbers less trustworthy.
ifeq ($(COVERAGE),1)
  CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -O0 -g -pthread --coverage
  COVERAGE_LDFLAGS := --coverage
else
  CXXFLAGS := -std=c++23 -Wall -Wextra -Wpedantic -Wshadow -O2 -g -pthread
endif
# All quoted includes are project-root-relative (e.g. "datamodel/kt_dm.h"),
# so the project root (this directory) is the only include path needed.
INCLUDES := -I.

# External libraries (oneTBB for parallelism, Boost.Iostreams/zlib for gzip
# input, fmt for log message formatting)
LIBS := -ltbb -lboost_iostreams -lz -lfmt

# Directories
SRC_DIR := .
# The coverage build gets its own tree: instrumented objects and counters must
# not sit next to the ordinary build's, or the ordinary binary ends up reporting
# as instrumented and a stale .gcda from an earlier run merges into the next.
ifeq ($(COVERAGE),1)
  BUILD_DIR := ../build-cov
else
  BUILD_DIR := ../build
endif
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := $(BUILD_DIR)/bin
LIB_DIR := $(BUILD_DIR)/lib

# Subdirectories
SUBDIRS := datamodel adaptor constraint placer legalizer detailPlacer visualization util

# Source files in current directory
LOCAL_SRCS := kt_flowMgr.cc kt_place.cc kt_option.cc

# ============================================================================
# Derived variables
# ============================================================================

# Object files for current directory
LOCAL_OBJS := $(LOCAL_SRCS:%.cc=$(OBJ_DIR)/%.o)

# Get object files from subdirectories via their Master.make files
# MAKEFLAGS is cleared for these. $(MAKE) inside $(shell ...) is not handed the
# jobserver, so every one of them announced "jobserver unavailable: using -j1"
# -- once per subdirectory, on every build. The target only echoes a list of
# paths, so dropping -j along with the warning costs nothing. The builds
# themselves go through the recipe at line 103, which does get the jobserver.
SUBDIR_OBJS := $(foreach dir,$(SUBDIRS),$(shell MAKEFLAGS= $(MAKE) -s -C $(dir) -f Master.make objlist OBJ_DIR=$(OBJ_DIR)/$(dir)))

# All object files
ALL_OBJS := $(LOCAL_OBJS) $(SUBDIR_OBJS)

# Dependency files
DEPS := $(ALL_OBJS:.o=.d)

# Final targets
TARGET := $(BIN_DIR)/ktplace
STATIC_LIB := $(LIB_DIR)/libktplace.a

# Engine objects without the entry point, so the library and the unit tests
# can link against them (kt_place.cc owns main()).
LIB_OBJS := $(filter-out $(OBJ_DIR)/kt_place.o,$(ALL_OBJS))

# ============================================================================
# Phony targets
# ============================================================================

.PHONY: all clean rebuild lib test test-build check $(SUBDIRS) dirs help print-subdirs

.DEFAULT_GOAL := all

# Default target
# Print the subdirectory list, one per line. The top-level Makefile asks for it
# when building compile_commands.json, so the database lists the same directories
# the build recurses into rather than a second copy of the list that can drift.
print-subdirs:
	@for d in $(SUBDIRS); do echo $$d; done

all: dirs $(SUBDIRS) $(TARGET) env

# Static library
lib: dirs $(SUBDIRS) $(STATIC_LIB)

# Create necessary directories
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR) $(LIB_DIR)
	@mkdir -p $(foreach dir,$(SUBDIRS),$(OBJ_DIR)/$(dir))

# Build subdirectories
$(SUBDIRS):
	@echo "Building $@..."
	@$(MAKE) -C $@ -f Master.make OBJ_DIR=$(OBJ_DIR)/$@ INCLUDES="$(INCLUDES)" CXX="$(CXX)" CXXFLAGS="$(CXXFLAGS)"

# Main executable
$(TARGET): $(ALL_OBJS) | dirs
	@echo "Linking $@..."
	@$(CXX) $(CXXFLAGS) -o $@ $(ALL_OBJS) $(LIBS) $(COVERAGE_LDFLAGS)
	@echo "Build complete: $@"

# Environment helper scripts.
#
# Copied to the repository root only after a successful link, so they never
# appear for a build that did not produce a binary. `cmp -s` keeps the copy
# from touching the timestamp when nothing changed, so sourcing stays cheap
# and make does not consider the target perpetually out of date.
TOP_DIR := $(abspath $(CURDIR)/..)
ENV_SCRIPTS := $(TOP_DIR)/ktplace.sh $(TOP_DIR)/ktplace.csh
ENV_SOURCES := $(TOP_DIR)/scripts/ktplace.sh $(TOP_DIR)/scripts/ktplace.csh

.PHONY: env
env: $(ENV_SCRIPTS)

# One rule per script, each copying only itself.
#
# This was one rule with a loop over both, and that is a race under `make -j`:
# make runs the recipe once per target, so two copies of the same loop run at
# once and both try to create both files, and the loser fails with "cp: cannot
# create regular file ... File exists". It only ever showed up once CI was given
# a parallel build, and the build has no reason to be serial.
$(TOP_DIR)/ktplace.sh: $(TOP_DIR)/scripts/ktplace.sh | $(TARGET)
	@if cmp -s "$<" "$@"; then echo "  up to date: $@"; \
	 else cp "$<" "$@" && echo "  generated: $@"; fi

$(TOP_DIR)/ktplace.csh: $(TOP_DIR)/scripts/ktplace.csh | $(TARGET)
	@if cmp -s "$<" "$@"; then echo "  up to date: $@"; \
	 else cp "$<" "$@" && echo "  generated: $@"; fi

# Static library
$(STATIC_LIB): $(LIB_OBJS) | dirs
	@echo "Creating static library $@..."
	@ar rcs $@ $(LIB_OBJS)
	@echo "Library created: $@"

# Compile local source files
# Object trees are nested (a module may live in a subdirectory, e.g.
# placer/simpl/), so the target directory is created before compiling -- gcc has
# to be able to write the generated .d file next to the .o.
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	@echo "Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# ============================================================================
# Unit tests (Boost.Test)
#
# One binary per component; each is a single translation unit that pulls in the
# Boost.Test runner, linked against the engine objects. Run with `make test`
# (alias: `make check`) from the repository root.
# ============================================================================

TEST_LIBS := -lboost_unit_test_framework

TEST_datamodel := datamodel/test/test_datamodel.cc
TEST_adaptor := adaptor/test/test_adaptor.cc
TEST_flow := test/test_flow.cc
TEST_util := util/test/test_util.cc
TEST_constraint := constraint/test/test_constraint.cc
TEST_option := test/test_option.cc
TEST_viz := visualization/test/test_viz.cc
TEST_detail := detailPlacer/test/test_detail.cc
TEST_ntuplace1 := placer/ntuplace1/test/test_ntuplace1.cc

TEST_BINS := $(BIN_DIR)/test_datamodel $(BIN_DIR)/test_adaptor $(BIN_DIR)/test_flow \
             $(BIN_DIR)/test_util $(BIN_DIR)/test_constraint $(BIN_DIR)/test_option $(BIN_DIR)/test_viz \
             $(BIN_DIR)/test_detail $(BIN_DIR)/test_ntuplace1

# Builds the test binaries without running them. `make coverage` needs this:
# the coverage driver wants to run the binaries itself, once, under the
# instrumented build, rather than have make run them before gcov is ready.
.PHONY: test-build
test-build: $(TEST_BINS)

.PHONY: test check
test check: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$(basename $$t) ---"; \
	    $$t --log_level=test_suite || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; fi; \
	echo "All unit tests passed."

# Each test binary: its own source plus the engine objects (no main()).
$(BIN_DIR)/test_datamodel: $(TEST_datamodel) $(LIB_OBJS) | dirs
	@echo "  Building test_datamodel..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_datamodel) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_adaptor: $(TEST_adaptor) $(LIB_OBJS) | dirs
	@echo "  Building test_adaptor..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_adaptor) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_flow: $(TEST_flow) $(LIB_OBJS) | dirs
	@echo "  Building test_flow..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_flow) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_util: $(TEST_util) $(LIB_OBJS) | dirs
	@echo "  Building test_util..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_util) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_constraint: $(TEST_constraint) $(LIB_OBJS) | dirs
	@echo "  Building test_constraint..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_constraint) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_option: $(TEST_option) $(LIB_OBJS) | dirs
	@echo "  Building test_option..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_option) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_viz: $(TEST_viz) $(LIB_OBJS) | dirs
	@echo "  Building test_viz..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_viz) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_detail: $(TEST_detail) $(LIB_OBJS) | dirs
	@echo "  Building test_detail..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_detail) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

$(BIN_DIR)/test_ntuplace1: $(TEST_ntuplace1) $(LIB_OBJS) | dirs
	@echo "  Building test_ntuplace1..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(TEST_ntuplace1) $(LIB_OBJS) $(LIBS) $(COVERAGE_LDFLAGS) $(TEST_LIBS)

# ============================================================================
# Line coverage
# ============================================================================
#
# Built and reported by scripts/coverage_report.py, which reads gcov's JSON and
# prints per-file and per-directory figures. gcov is used directly rather than
# lcov because it ships with gcc: a coverage gate that needs an extra package
# installed is a coverage gate that silently does not run.

.PHONY: coverage coverage-clean
coverage:
	@python3 ../scripts/coverage_report.py --clean --build \
		--min $(or $(COVERAGE_MIN),0)

coverage-clean:
	@rm -rf $(BUILD_DIR)-cov

# Clean
clean:
	@echo "Cleaning..."
	@rm -rf $(BUILD_DIR)
	@$(foreach dir,$(SUBDIRS),$(MAKE) -C $(dir) -f Master.make clean;)

# Rebuild
rebuild: clean all

# Help
help:
	@echo "KTPlace Build System"
	@echo "===================="
	@echo "Targets:"
	@echo "  all      - Build executable (default)"
	@echo "  lib      - Build static library"
	@echo "  test     - Build and run the unit tests (alias: check)"
	@echo "  lib      - Build static library"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and rebuild"
	@echo "  help     - Show this help message"
	@echo ""
	@echo "Configuration:"
	@echo "  CXX      = $(CXX)"
	@echo "  CXXFLAGS = $(CXXFLAGS)"
	@echo "  LIBS     = $(LIBS)"
	@echo "  Build dir = $(BUILD_DIR)"

# Object list for dependency tracking (used by parent makefiles)
objlist:
	@echo "$(LOCAL_OBJS)"


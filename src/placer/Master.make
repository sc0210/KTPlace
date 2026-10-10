# Master.make for placer subdirectory (placer algorithms).
# Each placer in this directory is a self-contained module compiled into the
# executable; add new .cc files to SRCS.

# ============================================================================
# Configuration
# ============================================================================

# Use variables passed from parent Makefile, or set defaults
CXX ?= g++
CXXFLAGS ?= -std=c++23 -Wall -Wextra -O2 -g -pthread
# Every source uses project-root-relative includes ("datamodel/kt_dm.h",
# "placer/simpl/kt_simpl.h", ...), so a single -I at the project root
# (src/) is all that is needed, regardless of which subdirectory make
# is running in.  `override` is required because the parent makefile
# passes INCLUDES on the command line.
override INCLUDES := -I..
OBJ_DIR ?= ../build/obj/placer

# Source files in this directory
SRCS := simpl/kt_simpl.cc ntuplace1/kt_ntuplace1.cc

# Only include files that exist
EXISTING_SRCS := $(foreach src,$(SRCS),$(if $(wildcard $(src)),$(src),))

# Object files
OBJS := $(EXISTING_SRCS:%.cc=$(OBJ_DIR)/%.o)

# Dependency files
DEPS := $(OBJS:.o=.d)

# ============================================================================
# Targets
# ============================================================================

.PHONY: all clean objlist

# Default: build objects. A placer module may live in a subdirectory (e.g.
# simpl/), so the object tree is nested; the directories are order-only
# prerequisites, created before any compile so the generated .d files have
# somewhere to go.
all: $(OBJS) | $(sort $(dir $(OBJS)))

$(sort $(dir $(OBJS))):
	@mkdir -p $@

# Compile source files
$(OBJ_DIR)/%.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  Compiling $<..."
	@$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

# Include dependency files
-include $(DEPS)

# ============================================================================
# Unit tests (Boost.Test)
# ============================================================================
#
# The suites (placer/simpl/test/test_simpl.cc) are built and run by the
# top-level `make test`, not here: they need ktDM, the logger, the
# timer and the plotter, so they link the whole engine and belong with the
# other suites at the top level.

# Clean this directory
clean:
	@rm -f $(OBJS) $(DEPS)

# Return object list (used by parent Makefile)
objlist:
	@echo "$(OBJS)"